#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

// Keeping PaletteShare.ini up to date. The shipped ini is embedded in the DLL (PaletteShare.rc), and
// [Display] IniVersion records the version that last wrote the file. SokuLauncher keeps the player's
// ini when it updates the mod, so when that's older than this build, the ini is rebuilt from the
// embedded one with the player's settings carried over: an upgraded ini has exactly the keys, order
// and comments of a fresh install. For each key in the shipped ini:
//   - set in the player's ini            -> Key=<their value> (also over a line shipped commented out)
//   - commented out in the player's ini  -> their commented line (a commented-out hotkey stays off)
//   - absent                             -> the shipped line (a key their version didn't have yet)
// Keys the shipped ini doesn't have (e.g. [Debug] ForcePaletteLeft) are kept at the end of their
// section and unknown sections at the end of the file; the player's own comments are not kept.
// Values whose meaning changed are fixed up first. An ini from a newer version is left alone, and
// so is a UTF-16 one (the game reads it either way).
namespace ps::ini {
	// The new text of an ini written by `from` (a version, "" for one without IniVersion).
	std::string upgraded(const std::string &shipped, const std::string &current, const std::string &from);
	// "a.b.c" as one comparable number (0 for "" or garbage).
	unsigned long long version(const std::string &text);
	// Upgrades `file` from the shipped ini embedded in `module`, if it's older (see above).
	void upgrade(HMODULE module, const std::filesystem::path &file);
}
