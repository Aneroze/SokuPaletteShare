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

	// A color as A8R8G8B8, whatever the palette's depth.
	static uint32_t argbAt(const Palette &p, int i)
	{
		if (p.bpp == 32) {
			uint32_t c;
			memcpy(&c, &p.colors[i * 4], 4);
			return c;
		}
		uint32_t v = p.colors[i * 2] | p.colors[i * 2 + 1] << 8;
		auto expand = [](uint32_t five) { return five << 3 | five >> 2; };
		return (v & 0x8000 ? 0xFF000000u : 0) | expand(v >> 10 & 31) << 16 | expand(v >> 5 & 31) << 8 | expand(v & 31);
	}

	Palette withColorOf(const Palette &palette, const Palette &from, int index)
	{
		Palette out = palette;
		if (!palette.valid() || !from.valid() || index < 0 || index > 255)
			return out;
		uint32_t c = argbAt(from, index);
		if (out.bpp == 32) {
			memcpy(&out.colors[index * 4], &c, 4);
		} else {
			uint16_t v = static_cast<uint16_t>((c >> 24 >= 0x80 ? 0x8000 : 0) | (c >> 19 & 31) << 10 | (c >> 11 & 31) << 5 | (c >> 3 & 31));
			memcpy(&out.colors[index * 2], &v, 2);
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
}
