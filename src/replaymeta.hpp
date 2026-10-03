#pragma once
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

// Per-replay metadata, a JSON object:
//
//   { "<ModName>": <anything that mod stores>, "<OtherMod>": ... }
//
// Each mod owns one top-level key named after itself, so mods don't need to know about each other.
// Two places to store it (chosen when saving; reading looks at both, the sidecar's keys win):
//
//   Sidecar: "<name>.meta.json" next to "<name>.rep". Saving merges our keys into whatever the file
//     already holds. The .rep is untouched; to share a replay with its metadata, share both files.
//   Rep: appended to the .rep after the game's data (which the game ignores), compact JSON then a
//     footer: u32 JSON size, u32 CRC-32 of the JSON, "SOKUMETA". Only written when the file ends
//     exactly where the game's data ends (input count at 0x77, then 2 bytes per input).
//
// Kept separate from the palette code so it can become its own mod that other mods call into.

namespace ps::meta {
	enum class Storage { Sidecar, Rep };
	void install(Storage storage);

	// A key's value for the replays saved from now on (until changed or cleared). Any thread.
	void setPending(const std::string &key, nlohmann::json value);
	void clearPending(const std::string &key);

	// A key of the replay being watched (read when the game loads a replay), if its sidecar has it.
	std::optional<nlohmann::json> watched(const std::string &key);
}
