#pragma once
#include <filesystem>
#include <string>

// A copy of the palette library outside the mod folder, so removing the mod (SokuLauncher deletes
// modules\PaletteShare) doesn't take the player's palettes with it.
//
// The library is modules\PaletteShare\palettes\<character>\ (easy to find, shipped with the mod).
// Its backup is <game folder>\PaletteShare\backup\<character>\, kept equal to it: new and changed
// files are copied over, and files gone from the library are moved to backup\_removed\<character>\
// (never deleted). A marker file in the library tells an installed library from a fresh one: when
// it's missing (a new install, or a reinstall after removing the mod), the backup is copied back
// first, newer files winning.
//
// Before 1.0.0, test builds kept the library in <game folder>\PaletteShare\palettes: its files are
// brought into the mod folder once (newer files winning), and the folder becomes the backup.
namespace ps::backup {
	struct Stats {
		int copied = 0;    // files copied
		int removed = 0;   // backup files moved to _removed
		int failed = 0;    // files that couldn't be copied or moved
	};

	constexpr const wchar_t *MARKER = L".paletteshare";
	constexpr const wchar_t *REMOVED = L"_removed";

	// At startup: migrate an old library, restore a fresh library from the backup, then back up the
	// whole library. `backupRoot` empty = no backup (the game folder isn't writable).
	void start(const std::filesystem::path &library, const std::filesystem::path &backupRoot,
		const std::filesystem::path &legacyLibrary);
	// Backs up one character's folder (after it changed). Does nothing without a backup.
	void character(const std::wstring &folderName);

	// The parts, for the tests. Copies every character folder's files from `from` to `to` where
	// `to` has no such file or an older one.
	Stats restore(const std::filesystem::path &from, const std::filesystem::path &to);
	// Makes `backup` (every character folder) equal to `library`, as described above.
	Stats mirror(const std::filesystem::path &library, const std::filesystem::path &backup);
	Stats mirrorCharacter(const std::filesystem::path &library, const std::filesystem::path &backup, const std::wstring &folderName);
}
