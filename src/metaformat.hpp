#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

// The file formats of per-replay metadata (see replaymeta.hpp), without any game or file access,
// so they can be tested on their own.
namespace ps::meta::format {
	constexpr size_t MAX_METADATA = 4 * 1024 * 1024;
	constexpr size_t FOOTER_SIZE = 16;          // u32 JSON size, u32 CRC-32 of the JSON, "SOKUMETA"
	constexpr size_t REPLAY_HEADER_SIZE = 0x7B; // the .rep's input count is at 0x77, inputs follow

	// Copying or printing a JSON value recurses once per nesting level, so a file nested millions of
	// levels deep (it only takes brackets) would overflow the stack: anything deeper is refused.
	constexpr int MAX_DEPTH = 32;
	bool shallowEnough(const std::string &text);

	// The text as a JSON object; nullopt when it isn't one, is too deep, or doesn't parse.
	std::optional<nlohmann::json> parseObject(const std::string &text);

	uint32_t crc32(const uint8_t *data, size_t size);

	// Where the game's data of a .rep ends (after its inputs), from its first REPLAY_HEADER_SIZE
	// bytes; nullopt when that's past the end of the file.
	std::optional<uint64_t> gameDataEnd(const uint8_t *header, size_t headerSize, uint64_t fileSize);

	// What to append to a .rep for this JSON text: the text and the footer.
	std::string makeTrailer(const std::string &json);

	// The JSON text appended to a whole .rep file, if it has a valid trailer starting exactly where
	// the game's data ends.
	std::optional<std::string> trailerOf(const uint8_t *file, size_t size);
}
