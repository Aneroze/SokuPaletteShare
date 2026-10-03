#include "sha256.hpp"
#include <cstdio>
#include <cstring>
#include <string>

namespace ps {
	namespace {
		constexpr uint32_t K[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
		};

		uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

		void block(uint32_t h[8], const uint8_t *p)
		{
			uint32_t w[64];
			for (int i = 0; i < 16; i++)
				w[i] = static_cast<uint32_t>(p[i * 4]) << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
			for (int i = 16; i < 64; i++) {
				uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
				uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
				w[i] = w[i - 16] + s0 + w[i - 7] + s1;
			}
			uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
			for (int i = 0; i < 64; i++) {
				uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
				uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
				k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
			}
			h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
		}
	}

	std::array<uint8_t, 32> sha256(const uint8_t *data, size_t size)
	{
		uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
		size_t full = size / 64 * 64;
		for (size_t at = 0; at < full; at += 64)
			block(h, data + at);
		// The rest, the 0x80 end marker, zeros, and the length in bits (big endian): 1 or 2 blocks.
		uint8_t tail[128] = {};
		size_t rest = size - full;
		if (rest)
			memcpy(tail, data + full, rest);
		tail[rest] = 0x80;
		size_t blocks = rest + 9 > 64 ? 2 : 1;
		uint64_t bits = static_cast<uint64_t>(size) * 8;
		for (int i = 0; i < 8; i++)
			tail[blocks * 64 - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
		for (size_t i = 0; i < blocks; i++)
			block(h, tail + i * 64);

		std::array<uint8_t, 32> out;
		for (int i = 0; i < 8; i++)
			for (int j = 0; j < 4; j++)
				out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
		return out;
	}

	bool sha256SelfTest()
	{
		struct Vector {
			std::string input;
			const char *hex;
		};
		const Vector vectors[] = {
			{"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
			{"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
			{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
			 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
			{std::string(1000, 'a'), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"},
		};
		for (auto &v : vectors) {
			auto digest = sha256(reinterpret_cast<const uint8_t *>(v.input.data()), v.input.size());
			char hex[65];
			for (int i = 0; i < 32; i++)
				snprintf(hex + i * 2, 3, "%02x", digest[i]);
			if (strcmp(hex, v.hex) != 0)
				return false;
		}
		return true;
	}
}
