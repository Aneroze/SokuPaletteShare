#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "palette.hpp"

// The player's own palettes: modules/PaletteShare/palettes/<character>/ holds .pal (game format, or
// 1024 raw 32-bit colors), .act (Photoshop color table) and .bmp (indexed color table, or the first
// 256 pixels of a true-color image) files. palettes.ini in the same folder stores the fallbacks:
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

	struct CharacterLibrary {
		int charId = -1;
		std::wstring folderName;
		std::filesystem::path folder;
		int defaultFallback = 0;
		std::vector<LibraryPalette> palettes;  // sorted by name
		std::wstring lastSelected;             // "" = a vanilla slot (see lastSlot)
		int lastSlot = -1;
	};

	void setLibraryRoot(const std::filesystem::path &root);
	// Loaded (and cached) on first use; rescanned when the folder changed since.
	const CharacterLibrary &library(int charId);
	void rememberSelection(int charId, const std::wstring &customName, int slot);

	std::optional<Palette> readPaletteFile(const std::filesystem::path &file);
}
