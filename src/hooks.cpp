#include "hooks.hpp"
#include "log.hpp"
#include <windows.h>
#include <SokuLib.hpp>

// How the game loads a character's palette (th123 1.10a):
//
//   character factory 0x46DE31: call 0x46C0B0   ; thiscall on the new v2::Player, no stack args
//     0x46C0F7: call 0x4679E0                   ; loads the character's .pat, passing
//                                               ; name(characterIndex) and paletteId
//       0x467AEB: sprintf "data/character/%s/palette%03d.bmp" (Okuu may use "...%03db.bmp")
//       0x467B7D: call 0x408BE0                 ; thiscall (ecx = 0x8A0048): load that file into
//                                               ; the single global palette 0x896B88 (ret 0xC)
//       ...sprites are then converted to textures through that palette, so the colors are
//       baked in here, once per character load.
//
// Hooking the factory's call tells us WHICH player is loading (v2::Player::teamId), so no
// guessing from load order. Hooking the palette file load lets us read what the game loaded
// and replace it before any sprite is converted.

namespace ps {
	Config config;
	PaletteCache cache;

	static void (__fastcall *orig_loadCharacter)(SokuLib::v2::Player *self, void *edx) = nullptr;
	static void (__fastcall *orig_loadPaletteFile)(void *self, void *edx, const char *name, SokuLib::Palette *palette, int bpp) = nullptr;

	static SokuLib::v2::Player *g_loadingPlayer = nullptr;

	static void __fastcall onLoadCharacter(SokuLib::v2::Player *self, void *edx)
	{
		g_loadingPlayer = self;
		orig_loadCharacter(self, edx);
		g_loadingPlayer = nullptr;
	}

	static void __fastcall onLoadPaletteFile(void *self, void *edx, const char *name, SokuLib::Palette *palette, int bpp)
	{
		orig_loadPaletteFile(self, edx, name, palette, bpp);

		// Only character loads (the "common" pattern and the select screen share this loader).
		auto player = g_loadingPlayer;
		if (!player || palette != &SokuLib::Palette::currentPalette)
			return;

		int side = player->teamId;
		auto loaded = readGamePalette(*palette);
		if (!loaded.valid()) {
			log("load side=%d char=%d slot=%d file=%s: palette unreadable (bpp=%d)",
				side, (int)player->characterIndex, (int)player->paletteId, name, (int)palette->bitsPerPixel);
			return;
		}

		auto hash = hashPalette(loaded);
		cache.store(loaded, hash);
		log("load side=%d char=%d slot=%d file=%s bpp=%d hash=%s",
			side, (int)player->characterIndex, (int)player->paletteId, name, (int)loaded.bpp, hash.hex().c_str());

		if (side < 0 || side > 1 || !config.forcePalette[side])
			return;

		auto forced = cache.load(*config.forcePalette[side]);
		if (!forced) {
			log("  force side=%d: %s not in cache (or corrupt), keeping the loaded palette",
				side, config.forcePalette[side]->hex().c_str());
			return;
		}
		writeGamePalette(*forced, *palette);
		log("  force side=%d: applied %s", side, config.forcePalette[side]->hex().c_str());
	}

	void installHooks()
	{
		// palette-picker rewrites the code before 0x467B7D and jumps over this load when a custom
		// palette is selected, so we would neither see nor replace that palette.
		if (GetModuleHandleA("palette-picker.dll"))
			log("WARNING: palette-picker is loaded; its custom palettes bypass PaletteShare's hook");

		DWORD old;
		VirtualProtect(reinterpret_cast<LPVOID>(TEXT_SECTION_OFFSET), TEXT_SECTION_SIZE, PAGE_EXECUTE_WRITECOPY, &old);
		orig_loadCharacter = SokuLib::TamperNearCall(0x46DE31, onLoadCharacter);
		orig_loadPaletteFile = SokuLib::TamperNearCall(0x467B7D, onLoadPaletteFile);
		VirtualProtect(reinterpret_cast<LPVOID>(TEXT_SECTION_OFFSET), TEXT_SECTION_SIZE, old, &old);
		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

		log("hooks installed (loadCharacter orig=%p, loadPaletteFile orig=%p)", orig_loadCharacter, orig_loadPaletteFile);
	}
}
