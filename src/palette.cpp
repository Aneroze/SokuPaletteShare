#include "palette.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <fstream>
#include <iterator>
#include <SokuLib.hpp>

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
		uint8_t digest[32] = {};
		BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, data.data(), static_cast<ULONG>(data.size()), digest, sizeof(digest));

		PaletteHash h;
		memcpy(h.bytes.data(), digest, h.bytes.size());
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

	bool trueColorSupported()
	{
		// DPhoenix's mod ships as PaletteTrueColors.dll; palette-picker looks for true-color-palettes.dll.
		return GetModuleHandleA("PaletteTrueColors.dll") || GetModuleHandleA("true-color-palettes.dll");
	}

	Palette readGamePalette(const SokuLib::Palette &game)
	{
		Palette p;
		p.bpp = game.bitsPerPixel;
		if (game.data && (p.bpp == 16 || p.bpp == 32)) {
			auto src = static_cast<const uint8_t *>(game.data);
			p.colors.assign(src, src + Palette::colorBytes(p.bpp));
		}
		return p;
	}

	static std::vector<uint8_t> to16bit(const std::vector<uint8_t> &argb32)
	{
		std::vector<uint8_t> out(512);
		for (int i = 0; i < 256; i++) {
			uint32_t c;
			memcpy(&c, &argb32[i * 4], 4);
			uint16_t a = (c >> 24) >= 0x80 ? 1 : 0;
			uint16_t v = (a << 15) | (((c >> 19) & 0x1F) << 10) | (((c >> 11) & 0x1F) << 5) | ((c >> 3) & 0x1F);
			memcpy(&out[i * 2], &v, 2);
		}
		return out;
	}

	void writeGamePalette(const Palette &palette, SokuLib::Palette &game)
	{
		if (!palette.valid())
			return;

		uint8_t bpp = palette.bpp;
		const std::vector<uint8_t> *colors = &palette.colors;
		std::vector<uint8_t> reduced;
		if (bpp == 32 && !trueColorSupported()) {
			reduced = to16bit(palette.colors);
			colors = &reduced;
			bpp = 16;
		}

		if (game.bitsPerPixel != bpp || !game.data) {
			if (game.data)
				SokuLib::DeleteFct(game.data);
			game.data = SokuLib::NewFct(Palette::colorBytes(bpp));
			game.bitsPerPixel = bpp;
		}
		memcpy(game.data, colors->data(), colors->size());
	}

	void PaletteCache::setDirectory(const std::filesystem::path &dir)
	{
		_dir = dir;
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);
	}

	bool PaletteCache::store(const Palette &palette, const PaletteHash &hash) const
	{
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
			if (!out)
				return false;
		}
		std::filesystem::rename(tmp, file, ec);
		return !ec;
	}

	std::optional<Palette> PaletteCache::load(const PaletteHash &hash) const
	{
		std::ifstream in(_dir / (hash.hex() + ".pal"), std::ios::binary);
		if (!in)
			return std::nullopt;

		std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		auto palette = Palette::deserialize(data.data(), data.size());
		if (!palette || !(hashPalette(*palette) == hash))
			return std::nullopt;
		return palette;
	}
}
