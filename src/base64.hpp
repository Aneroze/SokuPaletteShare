#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Standard base64 (RFC 4648, with padding), for palettes in replay metadata.
namespace ps::base64 {
	std::string encode(const std::vector<uint8_t> &data);
	// nullopt on a character outside the alphabet.
	std::optional<std::vector<uint8_t>> decode(const std::string &text);
}
