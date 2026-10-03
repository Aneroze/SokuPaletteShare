#include "base64.hpp"
#include <cstring>

namespace ps::base64 {
	namespace {
		const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	}

	std::string encode(const std::vector<uint8_t> &data)
	{
		std::string out;
		for (size_t i = 0; i < data.size(); i += 3) {
			uint32_t v = data[i] << 16 | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
			out += ALPHABET[v >> 18 & 63];
			out += ALPHABET[v >> 12 & 63];
			out += i + 1 < data.size() ? ALPHABET[v >> 6 & 63] : '=';
			out += i + 2 < data.size() ? ALPHABET[v & 63] : '=';
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
			auto p = c ? strchr(ALPHABET, c) : nullptr;
			if (!p)
				return std::nullopt;
			v = v << 6 | static_cast<uint32_t>(p - ALPHABET);
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				out.push_back(static_cast<uint8_t>(v >> bits));
			}
		}
		return out;
	}
}
