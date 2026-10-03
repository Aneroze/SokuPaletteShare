#pragma once
#include <windows.h>
#include <optional>
#include "palette.hpp"

namespace ps {
	struct Config {
		// [Display] ShowOpponentPalettes: start with the opponent's custom palette shown.
		bool showOpponentPalettes = true;
		// [Picker] ShowOriginalPalettes: for each slot a palette mod replaces, also list the game's
		// original colors for that slot ("Original NN"), usable like a custom palette.
		bool showOriginalPalettes = false;
		// [Replay] Storage: where saved replays keep their palettes: "sidecar" (<name>.meta.json next
		// to the replay, default) or "rep" (appended to the .rep itself).
		bool replayMetaInRep = false;
		// [Hotkey] Modifier + Key (virtual-key codes): toggle the opponent's custom palette, even
		// mid-match. Modifier 0 = key alone.
		int toggleModifier = VK_CONTROL;
		int toggleKey = 'P';
		// [Debug] ForcePaletteLeft / ForcePaletteRight: hash of a cached palette to show on that
		// side, as if the opponent had sent it (it goes through the same checks and the hotkey).
		std::optional<PaletteHash> forcePalette[2];
		// [Debug] HotkeyIgnoresFocus: react to the hotkey even when the game isn't the foreground
		// window (lets test scripts press a dummy key like F24 without stealing focus).
		bool hotkeyIgnoresFocus = false;
	};

	extern Config config;
	extern PaletteCache cache;

	void installHooks();

	// The opponent-palette hotkey (Ctrl+P): flips the setting when pressed this frame and says so.
	// Game thread, once per frame (battle and character select call it).
	bool pollOpponentToggle();
}
