#include "metaformat.hpp"
#include <cstring>

namespace ps::meta::format {
	namespace {
		constexpr size_t INPUT_COUNT_OFFSET = 0x77;  // u32, then 2 bytes per input
		constexpr char MAGIC[8] = {'S', 'O', 'K', 'U', 'M', 'E', 'T', 'A'};

		uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
		void put32(std::string &out, uint32_t v) { for (int i = 0; i < 4; i++) out += static_cast<char>(v >> (8 * i) & 0xFF); }
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

	uint32_t crc32(const uint8_t *data, size_t size)
	{
		uint32_t crc = 0xFFFFFFFF;
		for (size_t i = 0; i < size; i++) {
			crc ^= data[i];
			for (int k = 0; k < 8; k++)
				crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
		}
		return ~crc;
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

	std::string makeTrailer(const std::string &json)
	{
		std::string out = json;
		put32(out, static_cast<uint32_t>(json.size()));
		put32(out, crc32(reinterpret_cast<const uint8_t *>(json.data()), json.size()));
		out.append(MAGIC, 8);
		return out;
	}

	std::optional<std::string> trailerOf(const uint8_t *file, size_t size)
	{
		auto end = gameDataEnd(file, size, size);
		if (!end || size < *end + FOOTER_SIZE)
			return std::nullopt;
		const uint8_t *footer = file + size - FOOTER_SIZE;
		uint32_t length = get32(footer);
		if (memcmp(footer + 8, MAGIC, 8) != 0 || length > MAX_METADATA || *end + length + FOOTER_SIZE != size)
			return std::nullopt;
		if (crc32(file + *end, length) != get32(footer + 4))
			return std::nullopt;
		return std::string(reinterpret_cast<const char *>(file + *end), length);
	}
}
