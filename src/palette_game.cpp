#include "palette.hpp"
#include <windows.h>
#include <cstring>
#include <SokuLib.hpp>

// The palette functions that touch the game (its global palette object, its allocator), apart from
// palette.cpp so that one can be tested on its own.
namespace ps {
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
}
