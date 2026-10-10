#pragma once
#include <windows.h>
#include <filesystem>
#include <optional>
#include "palette.hpp"
#include "ui.hpp"

namespace ps {
	struct Config {
		// [Display] ShowOpponentPalettes: start with the opponent's custom palette shown.
		bool showOpponentPalettes = true;
		// [Picker] ShowOriginalPalettes: for each slot a palette mod replaces, also list the game's
		// original colors for that slot ("Original NN"), usable like a custom palette.
		bool showOriginalPalettes = false;
		// [Picker] FallbackButton: the game button that changes the highlighted palette's fallback
		// at character select (the card switch by default: D picks a random deck there).
		ui::Button fallbackButton = ui::Button::Switch;
		// [Save] AutoSave: save every custom palette an opponent plays with (see saved.hpp).
		bool autoSaveOpponents = true;
		// [Replay] SaveMetadata: save the match's custom palettes with replays (see replay.hpp).
		bool saveReplayPalettes = true;
		// [Hotkey] Modifier + Key (virtual-key codes): toggle the opponent's custom palette, even
		// mid-match. Modifier 0 = key alone.
		int toggleModifier = VK_CONTROL;
		int toggleKey = 'P';
		// [Hotkey] SaveModifier + SaveKey: save the opponent's palette now. None by default.
		int saveModifier = 0;
		int saveKey = 0;
		// [Debug] ForcePaletteLeft / ForcePaletteRight: a palette file to show on that side, as if
		// the opponent had sent it (it goes through the same checks and the hotkey).
		std::optional<Palette> forcePalette[2];
		// [Debug] HotkeyIgnoresFocus: react to the hotkey even when the game isn't the foreground
		// window (lets test scripts press a dummy key like F24 without stealing focus).
		bool hotkeyIgnoresFocus = false;
	};

	extern Config config;

	void installHooks();

	// The opponent-palette hotkey (Ctrl+P): flips the setting when pressed this frame and says so.
	// Game thread, once per frame (battle and character select call it).
	bool pollOpponentToggle();
	// The save hotkey: saves the opponent's custom palette when pressed this frame, the one their
	// character select preview shows (select) or their character in the match. Game thread, once per
	// frame.
	void pollSaveHotkey(bool select);
	// Draws the hotkeys' feedback ("Opponent palettes: ON/OFF", "Opponent palette saved as ...")
	// for 2 s (render thread, after the scene).
	void drawNotice();
	// Starts saving opponents' palettes to `folder` (saved.hpp), with the save hotkey's feedback.
	void startSaving(const std::filesystem::path &folder);
}
