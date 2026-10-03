#pragma once
#include <optional>
#include "palette.hpp"

// Palette picker at character select.
//
// In the palette step (selection stage 1: Up/Down = color, Left/Right = deck, C = submenu,
// D = random) the game's own palette cursor is a wheel of the 8 vanilla slots, driven by the
// local player's keys. In netplay both games replay those keys, so the slot the opponent's game
// computes for you comes from your key presses only.
//
// So for the local player we take over Up/Down: they move our list (8 vanilla slots + the
// library's custom palettes), and we steer the game's wheel to the highlighted entry's fallback
// slot with synthetic presses (KeymapManager::SetInputs call site 0x40A45D: the decoded input at
// +0x38 and the bits the netcode sends at +0x62). The opponent's game sees the same presses, so
// the fallback slot is exactly what they compute. Confirm (A) is held back until the wheel is
// on the fallback slot.

namespace ps::picker {
	void install();

	// The custom palette highlighted (character select) or confirmed (loading) for this player and
	// character, if it is a custom one. nullopt = a vanilla slot, or not ours to pick.
	std::optional<Palette> customFor(int player, int charId);
	// What the character-select preview of that player shows, built with the game's palette for
	// `slot` (the wheel's position): the highlighted entry's colors even while the wheel is still
	// stepping towards its slot. nullopt = the game's palette for that slot.
	std::optional<Palette> previewFor(int player, int charId, int slot);
}
