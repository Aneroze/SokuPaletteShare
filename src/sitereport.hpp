#pragma once
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

// Tells soku.cleista.cc which custom palette our side shows in an online match we play, so its live
// rooms and replays show it too. Only our own side: the opponent's palette is theirs to report.
// What's sent is our side of the replay sidecar value (replay.hpp) with the game's random seed.
// [Site] Upload=0 turns it off.
namespace ps::site {
	// Starts the sender thread (none when !upload). url: [Site] Url, empty for the site's.
	void start(bool upload, const std::wstring &url);
	// Stops it (game exit; waits at most 500 ms).
	void shutdown();
	// The replay sidecar value as it changes (replay.cpp; any thread, returns at once). Playing
	// online, our side's palette is sent ~1.5 s after the last change, once per seed and palette.
	void offer(const nlohmann::json &value);

	// The request body for our side (0 = p1, 1 = p2): the sidecar value cut down to that side's
	// palette; empty when that side has none (nothing to report).
	inline std::string body(uint32_t seed, int side, const nlohmann::json &value)
	{
		if (side < 0 || side > 1)
			return {};
		const char *key = side == 0 ? "p1" : "p2";
		auto palettes = value.find("palettes");
		if (palettes == value.end() || !palettes->is_object())
			return {};
		auto ours = palettes->find(key);
		if (ours == palettes->end())
			return {};
		auto version = value.find("version");
		nlohmann::json report = {{"version", version != value.end() ? *version : nlohmann::json()}, {"palettes", {{key, *ours}}}};
		return nlohmann::json{{"seed", seed}, {"reporter", key}, {"PaletteShare", report}}.dump();
	}
}
