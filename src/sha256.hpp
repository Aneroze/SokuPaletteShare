#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// SHA-256 (FIPS 180-4), built in so hashing doesn't depend on the OS (Windows 7, any Wine) and can
// be tested outside the game.
namespace ps {
	std::array<uint8_t, 32> sha256(const uint8_t *data, size_t size);
	// Checks the implementation against the standard test vectors.
	bool sha256SelfTest();
}
