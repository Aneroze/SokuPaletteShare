#pragma once
#include <optional>
#include "palette.hpp"

// Palette exchange with the opponent in netplay, and with spectators.
//
// While both players are in a netplay session (character select, loading, battle), each side
// offers the palette its own character is shown with: captured from the character-select
// preview as the player browses, and again when the character loads. A modded peer keeps what
// it receives and uses it when that character loads (spectators get both sides from the game
// they watch through). Peers without the mod ignore the packets (see protocol.hpp), and nothing
// is sent to them after the handshake times out.

namespace ps::net {
	void install();
	// Stops the background thread (game exit).
	void shutdown();

	// Side of the local player in a netplay match as a player (0 = host/left, 1 = client/right),
	// -1 otherwise (offline, spectating, replays).
	int localSide();

	// Main thread: the palette the local player's character is shown with.
	// again: send it once more even if it's the current offer and was acknowledged (at load).
	void offerLocal(int charId, int slot, const Palette &palette, const PaletteHash &hash, bool again = false);

	// What we received for a side (the opponent's, or either when spectating): the palette for this
	// character and slot, if any.
	std::optional<Palette> received(int side, int charId, int slot);
	// The palette that side shows this character with right now, whatever the slot (at character
	// select the opponent's slot moves one step at a time towards the one their palette falls back to).
	std::optional<Palette> receivedNow(int side, int charId);

	enum class PeerStatus {
		NotInNetplay,
		Checking,      // session just started, no answer yet
		HasMod,        // the opponent runs a compatible PaletteShare
		NoMod,         // no answer after a few seconds: they see your fallback slot
		Incompatible,  // they run another protocol version: they see your fallback slot
	};
	PeerStatus peerStatus();

	// Player in netplay: a side's character loaded (own = our palette for our side). Spectators
	// watching through us get what each side is shown with.
	void sideLoaded(int side, int charId, int slot, const Palette *own);

	// Bumped whenever a new remote palette has been received; lets the renderer pick up a palette
	// that arrived after the character had already loaded.
	unsigned remoteGeneration();
}
