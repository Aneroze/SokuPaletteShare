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
//   fallback=0        ; slot used by palettes that have no fallback of their own (default 0)
//   [Blue Reimu]      ; file name without extension
//   fallback=3
//
// The fallback is the vanilla slot (0-7) the game actually sends, so it is what players without
// PaletteShare (and spectators/replays) see.

namespace ps {
	struct LibraryPalette {
		std::wstring name;            // file name without extension
		std::filesystem::path file;
		Palette palette;
		PaletteHash hash;
		int fallback = 0;
		bool fallbackIsDefault = true;  // no fallback of its own; uses the character default
	};

	struct SkippedFile {
		std::wstring file;            // file name with extension
		std::string reason;
	};

	struct CharacterLibrary {
		int charId = -1;
		std::wstring folderName;
		std::filesystem::path folder;
		int defaultFallback = 0;
		std::vector<LibraryPalette> palettes;  // sorted by name
		std::vector<SkippedFile> skipped;      // palette-like files that couldn't be read
		std::wstring lastSelected;             // "" = a vanilla slot (see lastSlot)
		int lastSlot = -1;
	};

	void setLibraryRoot(const std::filesystem::path &root);
	// Loaded (and cached) on first use; rescanned when the folder changed since.
	const CharacterLibrary &library(int charId);
	void rememberSelection(int charId, const std::wstring &customName, int slot);
	// Saves a palette's own fallback slot (0-7) to palettes.ini.
	void setFallback(int charId, const std::wstring &name, int slot);
	// Removes a palette's own fallback: it follows the character default again.
	void clearFallback(int charId, const std::wstring &name);
	// The slot used by palettes without their own fallback.
	void setDefaultFallback(int charId, int slot);

	// whyNot (optional) gets a short reason when the file can't be read.
	std::optional<Palette> readPaletteFile(const std::filesystem::path &file, const char **whyNot = nullptr);
}
