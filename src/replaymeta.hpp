#pragma once
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

// Per-replay metadata, a JSON object:
//
//   { "<ModName>": <anything that mod stores>, "<OtherMod>": ... }
//
// Each mod owns one top-level key named after itself, so mods don't need to know about each other.
// It's stored in "<name>.meta.json" next to "<name>.rep"; saving merges our keys into whatever the
// file already holds. The .rep is untouched; to share a replay with its metadata, share both files.
// (Until 1.0.0's release it could also be appended to the .rep: see the git tag rep-storage-last.)
//
// Kept separate from the palette code so it can become its own mod that other mods call into.

namespace ps::meta {
	void install();

	// A key's value for the replays saved from now on (until changed or cleared). Any thread.
	void setPending(const std::string &key, nlohmann::json value);
	void clearPending(const std::string &key);

	// A key of the replay being watched (read when the game loads a replay), if its sidecar has it.
	std::optional<nlohmann::json> watched(const std::string &key);
}
