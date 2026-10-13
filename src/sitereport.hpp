#pragma once
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

// Tells soku.cleista.cc which custom palettes an online match shows (as a player or spectating),
// so its live rooms and replays show them too. What's sent is our replay sidecar value (replay.hpp)
// with the game's random seed and the reporter's side. [Site] Upload=0 turns it off.
namespace ps::site {
	// Starts the sender thread (none when !upload). url: [Site] Url, empty for the site's.
	void start(bool upload, const std::wstring &url);
	// Stops it (game exit; waits at most 500 ms).
	void shutdown();
	// The replay sidecar value as it changes (replay.cpp; any thread, returns at once). Online, a
	// value with palettes is sent ~1.5 s after the last change, once per seed and value.
	void offer(const nlohmann::json &value);

	// The request body; empty when the value has no palettes (nothing to report).
	inline std::string body(uint32_t seed, const char *reporter, const nlohmann::json &value)
	{
		auto palettes = value.find("palettes");
		if (palettes == value.end() || !palettes->is_object() || palettes->empty())
			return {};
		return nlohmann::json{{"seed", seed}, {"reporter", reporter}, {"PaletteShare", value}}.dump();
	}
}
