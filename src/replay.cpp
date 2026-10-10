#include "replay.hpp"
#include "base64.hpp"
#include "log.hpp"
#include "replaymeta.hpp"
#include <cstring>
#include <windows.h>
#include <mutex>
#include <string>
#include <vector>

namespace ps::replay {
	namespace {
		constexpr const char *KEY = "PaletteShare";
		constexpr int VERSION = 1;
		const char *const SIDES[2] = {"p1", "p2"};

		struct Entry {
			int charId = -1;
			int slot = -1;
			std::optional<Palette> palette;
		};

		std::mutex g_mutex;
		Entry g_recorded[2];
		bool g_saving = true;

		std::optional<int> intField(const nlohmann::json &object, const char *key)
		{
			auto it = object.find(key);
			if (it == object.end() || !it->is_number_integer())
				return std::nullopt;
			return it->get<int>();
		}

		// Our key's value for the replays saved from now on (caller holds g_mutex).
		void publish()
		{
			nlohmann::json palettes = nlohmann::json::object();
			for (int side = 0; side < 2; side++) {
				auto &e = g_recorded[side];
				if (e.palette && e.palette->valid())
					palettes[SIDES[side]] = {{"character", e.charId}, {"slot", e.slot}, {"palette", base64::encode(e.palette->serialize())}};
			}
			// Off: cleared rather than left alone, so a sidecar left by a deleted replay of the same
			// name doesn't keep our data from that other match.
			if (palettes.empty() || !g_saving)
				meta::clearPending(KEY);
			else
				meta::setPending(KEY, {{"version", VERSION}, {"palettes", palettes}});
		}
	}

	void record(int side, int charId, int slot, const std::optional<Palette> &custom, bool atLoad)
	{
		if (side < 0 || side > 1)
			return;
		std::lock_guard lock(g_mutex);
		static ULONGLONG lastLoad = 0;
		if (atLoad) {
			ULONGLONG now = GetTickCount64();
			if (now - lastLoad > 5000)
				g_recorded[0] = g_recorded[1] = Entry{};  // a new match
			lastLoad = now;
		}
		g_recorded[side] = {charId, slot, custom};
		publish();
	}

	void setSaving(bool on)
	{
		std::lock_guard lock(g_mutex);
		g_saving = on;
		publish();
	}

	std::optional<Palette> paletteFor(int side, int charId, int slot)
	{
		if (side < 0 || side > 1)
			return std::nullopt;
		// Replays come from anywhere: a malformed file must never take the game down.
		try {
			return recorded(side, charId, slot);
		} catch (...) {
			log("replay: the watched replay's PaletteShare data is malformed, ignored");
			return std::nullopt;
		}
	}

	std::optional<Palette> recorded(int side, int charId, int slot)
	{
		auto ours = meta::watched(KEY);
		if (!ours || !ours->is_object() || intField(*ours, "version") != VERSION)
			return std::nullopt;
		auto palettes = ours->find("palettes");
		if (palettes == ours->end() || !palettes->is_object())
			return std::nullopt;
		auto entry = palettes->find(SIDES[side]);
		if (entry == palettes->end() || !entry->is_object())
			return std::nullopt;
		if (intField(*entry, "character") != charId || intField(*entry, "slot") != slot)
			return std::nullopt;
		auto text = entry->find("palette");
		if (text == entry->end() || !text->is_string())
			return std::nullopt;
		auto bytes = base64::decode(text->get<std::string>());
		auto palette = bytes ? Palette::deserialize(bytes->data(), bytes->size()) : std::nullopt;
		if (palette)
			log("replay: side=%d char=%d slot=%d has a recorded palette %s", side, charId, slot, hashPalette(*palette).hex().c_str());
		return palette;
	}
}
