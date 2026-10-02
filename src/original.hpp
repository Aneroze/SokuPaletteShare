#pragma once
#include <optional>
#include "palette.hpp"

// The game's own palettes, read straight from its .dat archives (th105a/b, th123a/b/c), so
// shady-loader packages, loose data files and other overrides don't apply. That's what a player
// without PaletteShare and without palette mods sees for a slot: our best guess for "Others".
namespace ps::original {
	// charName is the game's internal name (e.g. "reimu"); slot 0-7. Read once, then cached.
	// nullopt when the archives can't be read (the caller falls back to the game's loader).
	std::optional<Palette> palette(const char *charName, int slot);
}
