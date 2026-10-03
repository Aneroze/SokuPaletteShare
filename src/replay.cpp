#include "replay.hpp"
#include "log.hpp"
#include "replaymeta.hpp"
#include <cstring>
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

		const char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		std::string encode(const std::vector<uint8_t> &data)
		{
			std::string out;
			for (size_t i = 0; i < data.size(); i += 3) {
				uint32_t v = data[i] << 16 | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
				out += BASE64[v >> 18 & 63];
				out += BASE64[v >> 12 & 63];
				out += i + 1 < data.size() ? BASE64[v >> 6 & 63] : '=';
				out += i + 2 < data.size() ? BASE64[v & 63] : '=';
			}
			return out;
		}

		std::optional<std::vector<uint8_t>> decode(const std::string &text)
		{
			std::vector<uint8_t> out;
			uint32_t v = 0;
			int bits = 0;
			for (char c : text) {
				if (c == '=')
					break;
				auto p = strchr(BASE64, c);
				if (!p || !c)
					return std::nullopt;
				v = v << 6 | static_cast<uint32_t>(p - BASE64);
				bits += 6;
				if (bits >= 8) {
					bits -= 8;
					out.push_back(static_cast<uint8_t>(v >> bits));
				}
			}
			return out;
		}

		// Our key's value for the replays saved from now on (caller holds g_mutex).
		void publish()
		{
			nlohmann::json palettes = nlohmann::json::object();
			for (int side = 0; side < 2; side++) {
				auto &e = g_recorded[side];
				if (e.palette && e.palette->valid())
					palettes[SIDES[side]] = {{"character", e.charId}, {"slot", e.slot}, {"palette", encode(e.palette->serialize())}};
			}
			if (palettes.empty())
				meta::clearPending(KEY);
			else
				meta::setPending(KEY, {{"version", VERSION}, {"palettes", palettes}});
		}
	}

	void record(int side, int charId, int slot, const std::optional<Palette> &custom)
	{
		if (side < 0 || side > 1)
			return;
		std::lock_guard lock(g_mutex);
		g_recorded[side] = {charId, slot, custom};
		publish();
	}

	std::optional<Palette> paletteFor(int side, int charId, int slot)
	{
		if (side < 0 || side > 1)
			return std::nullopt;
		auto ours = meta::watched(KEY);
		if (!ours || !ours->is_object() || ours->value("version", 0) != VERSION)
			return std::nullopt;
		auto palettes = ours->find("palettes");
		if (palettes == ours->end() || !palettes->is_object())
			return std::nullopt;
		auto entry = palettes->find(SIDES[side]);
		if (entry == palettes->end() || !entry->is_object())
			return std::nullopt;
		if (entry->value("character", -1) != charId || entry->value("slot", -1) != slot)
			return std::nullopt;
		auto text = entry->find("palette");
		if (text == entry->end() || !text->is_string())
			return std::nullopt;
		auto bytes = decode(text->get<std::string>());
		auto palette = bytes ? Palette::deserialize(bytes->data(), bytes->size()) : std::nullopt;
		if (palette)
			log("replay: side=%d char=%d slot=%d has a recorded palette %s", side, charId, slot, hashPalette(*palette).hex().c_str());
		return palette;
	}
}
