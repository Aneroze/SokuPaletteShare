#include "metaformat.hpp"
#include <cstring>

namespace ps::meta::format {
	namespace {
		constexpr size_t INPUT_COUNT_OFFSET = 0x77;  // u32, then 2 bytes per input

		uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
	}

	bool shallowEnough(const std::string &text)
	{
		int depth = 0;
		bool inString = false, escaped = false;
		for (char c : text) {
			if (inString) {
				if (escaped)
					escaped = false;
				else if (c == '\\')
					escaped = true;
				else if (c == '"')
					inString = false;
			} else if (c == '"') {
				inString = true;
			} else if (c == '{' || c == '[') {
				if (++depth > MAX_DEPTH)
					return false;
			} else if (c == '}' || c == ']') {
				depth--;
			}
		}
		return true;
	}

	std::optional<nlohmann::json> parseObject(const std::string &text)
	{
		if (text.size() > MAX_METADATA || !shallowEnough(text))
			return std::nullopt;
		auto parsed = nlohmann::json::parse(text, nullptr, false);
		if (parsed.is_discarded() || !parsed.is_object())
			return std::nullopt;
		return parsed;
	}

	std::optional<uint64_t> gameDataEnd(const uint8_t *header, size_t headerSize, uint64_t fileSize)
	{
		if (headerSize < REPLAY_HEADER_SIZE || fileSize < REPLAY_HEADER_SIZE)
			return std::nullopt;
		uint64_t end = REPLAY_HEADER_SIZE + uint64_t(get32(header + INPUT_COUNT_OFFSET)) * 2;
		if (end > fileSize)
			return std::nullopt;
		return end;
	}
}
