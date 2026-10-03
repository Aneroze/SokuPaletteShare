#include "palette.hpp"
#include "guard.hpp"
#include "sha256.hpp"
#include "log.hpp"
#include <algorithm>
#include <windows.h>
#include <fstream>
#include <iterator>

namespace ps {
	std::vector<uint8_t> Palette::serialize() const
	{
		std::vector<uint8_t> out;
		out.reserve(1 + colors.size());
		out.push_back(bpp);
		out.insert(out.end(), colors.begin(), colors.end());
		return out;
	}

	std::optional<Palette> Palette::deserialize(const uint8_t *data, size_t size)
	{
		if (size < 1)
			return std::nullopt;

		Palette p;
		p.bpp = data[0];
		if ((p.bpp != 16 && p.bpp != 32) || size != 1 + colorBytes(p.bpp))
			return std::nullopt;
		p.colors.assign(data + 1, data + size);
		return p;
	}

	std::string PaletteHash::hex() const
	{
		static const char digits[] = "0123456789abcdef";
		std::string out;
		for (auto b : bytes) {
			out += digits[b >> 4];
			out += digits[b & 0xF];
		}
		return out;
	}

	std::optional<PaletteHash> PaletteHash::fromHex(const std::string &hex)
	{
		auto nibble = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};

		if (hex.size() != 32)
			return std::nullopt;

		PaletteHash h;
		for (size_t i = 0; i < 16; i++) {
			int hi = nibble(hex[i * 2]);
			int lo = nibble(hex[i * 2 + 1]);
			if (hi < 0 || lo < 0)
				return std::nullopt;
			h.bytes[i] = static_cast<uint8_t>(hi << 4 | lo);
		}
		return h;
	}

	PaletteHash hashPalette(const Palette &palette)
	{
		auto data = palette.serialize();
		auto digest = sha256(data.data(), data.size());

		PaletteHash h;
		memcpy(h.bytes.data(), digest.data(), h.bytes.size());
		return h;
	}

	static bool opaqueAt(const Palette &p, int i)
	{
		if (p.bpp == 16)
			return (p.colors[i * 2 + 1] & 0x80) != 0;
		return p.colors[i * 4 + 3] >= 0x80;
	}

	Palette lockTransparency(const Palette &custom, const Palette &reference)
	{
		Palette out = custom;
		if (!custom.valid() || !reference.valid())
			return out;

		for (int i = 0; i < 256; i++) {
			bool opaque = opaqueAt(reference, i);
			if (out.bpp == 16) {
				if (opaque)
					out.colors[i * 2 + 1] |= 0x80;
				else
					out.colors[i * 2 + 1] &= 0x7F;
			} else {
				out.colors[i * 4 + 3] = opaque ? 0xFF : 0x00;
			}
		}
		return out;
	}

	static uint16_t rgb555At(const Palette &p, int i)
	{
		if (p.bpp == 16)
			return static_cast<uint16_t>((p.colors[i * 2] | p.colors[i * 2 + 1] << 8) & 0x7FFF);
		uint32_t c;
		memcpy(&c, &p.colors[i * 4], 4);
		return static_cast<uint16_t>(((c >> 19) & 0x1F) << 10 | ((c >> 11) & 0x1F) << 5 | ((c >> 3) & 0x1F));
	}

	bool sameColors(const Palette &a, const Palette &b)
	{
		if (!a.valid() || !b.valid())
			return false;
		for (int i = 0; i < 256; i++)
			if (rgb555At(a, i) != rgb555At(b, i))
				return false;
		return true;
	}

	void PaletteCache::setDirectory(const std::filesystem::path &dir)
	{
		_dir = dir;
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);

		std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> palettes;
		std::vector<std::filesystem::path> unfinished;
		forEachEntry(_dir, [&](const std::filesystem::directory_entry &entry) {
			std::error_code e;
			auto ext = entry.path().extension();
			if (ext == L".tmp")
				unfinished.push_back(entry.path());  // a write that never finished
			else if (ext == L".pal")
				palettes.emplace_back(entry.last_write_time(e), entry.path());
		});
		for (auto &file : unfinished)
			std::filesystem::remove(file, ec);
		if (palettes.size() <= MAX_FILES)
			return;
		std::sort(palettes.begin(), palettes.end(), [](auto &a, auto &b) { return a.first > b.first; });
		for (size_t i = KEEP_FILES; i < palettes.size(); i++)
			std::filesystem::remove(palettes[i].second, ec);
		log("cache: trimmed %d old palette(s)", (int)(palettes.size() - KEEP_FILES));
	}

	void PaletteCache::keep(const Palette &palette, const PaletteHash &hash)
	{
		std::lock_guard lock(_mutex);
		if (_memory.size() >= MAX_IN_MEMORY)
			_memory.clear();
		_memory[hash.bytes] = palette;
	}

	bool PaletteCache::store(const Palette &palette, const PaletteHash &hash)
	{
		keep(palette, hash);
		auto file = _dir / (hash.hex() + ".pal");
		std::error_code ec;
		if (std::filesystem::exists(file, ec))
			return true;

		// Write to a temp name first so a crash never leaves a truncated palette under a valid hash.
		auto tmp = file;
		tmp += L".tmp";
		{
			std::ofstream out(tmp, std::ios::binary);
			auto data = palette.serialize();
			out.write(reinterpret_cast<const char *>(data.data()), data.size());
		}
		if (std::filesystem::file_size(tmp, ec) != palette.serialize().size()) {
			std::filesystem::remove(tmp, ec);
			return false;  // e.g. a full disk; the palette still works this session (in memory)
		}
		std::filesystem::rename(tmp, file, ec);
		if (ec)
			std::filesystem::remove(tmp, ec);
		return !ec;
	}

	std::optional<Palette> PaletteCache::load(const PaletteHash &hash)
	{
		{
			std::lock_guard lock(_mutex);
			auto it = _memory.find(hash.bytes);
			if (it != _memory.end())
				return it->second;
		}
		auto file = _dir / (hash.hex() + ".pal");
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return std::nullopt;

		std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();
		auto palette = Palette::deserialize(data.data(), data.size());
		if (!palette || !(hashPalette(*palette) == hash)) {
			// Damaged: remove it, or store() would never replace it (it skips existing files).
			std::error_code ec;
			std::filesystem::remove(file, ec);
			log("cache: %s was damaged, removed", hash.hex().c_str());
			return std::nullopt;
		}
		keep(*palette, hash);
		return palette;
	}
}
