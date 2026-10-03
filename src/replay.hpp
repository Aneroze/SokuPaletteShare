#pragma once
#include <optional>
#include "palette.hpp"

// Custom palettes in replays, stored in the replay's metadata sidecar (see replaymeta.hpp) under
// the key "PaletteShare":
//
//   "PaletteShare": {
//     "version": 1,
//     "palettes": {
//       "p1": { "character": 0, "slot": 7, "palette": "<base64 of the .pal: bpp byte + colors>" },
//       "p2": { ... }
//     }
//   }
//
// p1/p2 are the left/right sides; a side without a custom palette is left out. On playback a
// palette is used only for the character and slot it was recorded with.

namespace ps::replay {
	// What a side of the match being played shows as its custom palette (nullopt: the game's own
	// palette for its slot). Saved into the replays saved from now on. Any thread.
	// atLoad: recorded as the character loads. The first load of a new match (none for 5 s) forgets
	// the last match's entries first, so a side whose load we don't see can't keep a stale one.
	void record(int side, int charId, int slot, const std::optional<Palette> &custom, bool atLoad = false);

	// The custom palette the replay being watched recorded for this side, if it was recorded for
	// this character and slot.
	std::optional<Palette> paletteFor(int side, int charId, int slot);
	// paletteFor without the safety net (may throw on malformed data).
	std::optional<Palette> recorded(int side, int charId, int slot);
}
