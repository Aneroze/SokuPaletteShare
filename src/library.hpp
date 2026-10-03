#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "palette.hpp"

// The player's own palettes: modules/PaletteShare/palettes/<character>/ holds .pal (game format,
// JASC-PAL, RIFF PAL, or the old Palette Editor's 256 raw 32-bit colors), .act (Photoshop color
// table) and .bmp (indexed color table, or the first 256 pixels of a 16/24/32-bit image) files.
// Files that can't be read are listed with the reason, so the player can see why. palettes.ini in the same folder stores the fallbacks:
//
//   [default]
//   fallback=0               ; slot used by palettes that have no fallback of their own (default 0)
//   [palette:Blue Reimu]     ; file name without extension (before 1.0.0: [Blue Reimu], still read)
//   fallback=3
//   [last] / [last2p]        ; the last choice of player 1 (our side online) / player 2 offline
//
// The file is UTF-16 (with a byte order mark) so any file name survives; an ANSI one is converted
// the first time we write to it.
//
// The fallback is the vanilla slot (0-7) the game actually sends, so it is what players without
// PaletteShare (and spectators/replays) see.
//
// Slots a palette mod replaces in this game (a shady-loader package, data files) can be used like
// library palettes too: addModdedSlots adds one "Mod NN" entry per replaced slot, whose fallback
// is that same slot unless the player sets another ([*mod NN] in palettes.ini; '*' can't appear
// in a file name, so these never clash with a file's section).

namespace ps {
	struct LibraryPalette {
		std::wstring name;            // file name without extension
		std::filesystem::path file;
		Palette palette;
		PaletteHash hash;
		int fallback = 0;
		bool fallbackIsDefault = true;  // no fallback of its own; uses the character default
		int modSlot = -1;               // >= 0: not a file but this game's (modded) palette for that slot
		int originalSlot = -1;          // >= 0: not a file but the game's original colors for that slot
	};

	struct SkippedFile {
		std::wstring file;            // file name with extension
		std::string reason;
	};

	struct CharacterLibrary {
		int charId = -1;
		std::wstring folderName;
		std::filesystem::path folder;     // the library's folder: palettes.ini lives here
		std::filesystem::path modFolder;  // the mod folder's palettes/<character>, also read (or empty)
		int defaultFallback = 0;
		std::vector<LibraryPalette> palettes;  // sorted by name
		std::vector<SkippedFile> skipped;      // palette-like files that couldn't be read
		// Last choice, [0] the player's own (player 1, or our side online), [1] player 2's offline.
		std::wstring lastSelected[2];          // "" = a vanilla slot (see lastSlot)
		int lastSlot[2] = {-1, -1};
	};

	// Palettes are read from `main` (<game folder>\PaletteShare\palettes when possible, which
	// removing or updating the mod never touches) and from `mod` (modules\PaletteShare\palettes); a file in
	// `main` wins over one of the same name in `mod`. palettes.ini is kept in `main`; the first time
	// a character is used there, the mod folder's palettes.ini is copied over.
	void setLibraryRoots(const std::filesystem::path &main, const std::filesystem::path &mod);
	// Loaded (and cached) on first use; rescanned when the folder changed since.
	const CharacterLibrary &library(int charId);
	// who: 0 = the player's own choice, 1 = player 2 offline.
	void rememberSelection(int charId, int who, const std::wstring &customName, int slot);
	// Saves a palette's own fallback slot (0-7) to palettes.ini.
	void setFallback(int charId, const std::wstring &name, int slot);
	// Removes a palette's own fallback: it follows the character default again.
	void clearFallback(int charId, const std::wstring &name);
	// The slot used by palettes without their own fallback.
	void setDefaultFallback(int charId, int slot);

	// Adds a "Mod NN" entry in front of the library's palettes for each slot this game shows
	// differently from the original (see above), and with `originals` an "Original NN" entry with
	// the original colors of each such slot (always sent as that slot: no fallback of its own).
	// Game thread (it loads the slots through the game).
	void addModdedSlots(CharacterLibrary &lib, bool originals);
	// The name to show: the file name, "Mod NN" or "Original NN".
	std::wstring displayName(const LibraryPalette &palette);

	// whyNot (optional) gets a short reason when the file can't be read.
	std::optional<Palette> readPaletteFile(const std::filesystem::path &file, const char **whyNot = nullptr);
}
