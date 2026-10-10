#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include "palette.hpp"

// Opponents' custom palettes, saved as palette files: one folder (<game folder>\PaletteShare\saved,
// see main.cpp) holding <character>-<profile>-NN.pal, where character is the game's internal name
// ("udonge"), profile the opponent's profile name and NN counts that player's palettes for that
// character (01, 02, ...). The files are in the game's own .pal format, so they can be copied into
// the library as they are. A palette already saved for that character and player isn't saved
// again. No game access, so it can be tested on its own.
namespace ps::saved {
	struct Result {
		enum Kind { Saved, AlreadySaved, Failed } kind = Failed;
		std::wstring file;  // file name with extension (the existing one for AlreadySaved)
	};

	// Starts the writer thread; files go to `folder` (created here). `onNotify` gets the result of
	// each save asked with notify (on the writer thread).
	void start(const std::filesystem::path &folder, std::function<void(const Result &)> onNotify);
	// Stops the writer thread once the queued saves are written (game exit; waits at most 1 s).
	void shutdown();
	// Queues a save; returns at once (any thread). character: internal name, player: profile name
	// as the game has it, read with decodeName(player, playerCodePage, the system's code page).
	void save(const std::string &character, const std::string &player, unsigned playerCodePage, const Palette &palette, bool notify);

	// The parts, for the tests.
	// A profile name from netplay: bytes in the code page of the Windows it came from, which nothing
	// says. Read in codePage (the game's text code page, see game::textCodePage) if it's valid
	// there, else in fallbackCodePage (this system's): a Shift-JIS name and a Western "Café" both
	// come out right on a Western system without th123intl, a GBK name with th123intl in Chinese.
	std::wstring decodeName(const std::string &name, unsigned codePage, unsigned fallbackCodePage);
	// A name usable in a file name: no characters Windows forbids there (they become '_'), no
	// leading or trailing blanks or dots; "unknown" if nothing is left.
	std::wstring cleanName(const std::wstring &name);
	// Writes the palette to `folder` unless that character and player already have it there.
	Result saveNow(const std::filesystem::path &folder, const std::wstring &character, const std::wstring &player, const Palette &palette);
}
