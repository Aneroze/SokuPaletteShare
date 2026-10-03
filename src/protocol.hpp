#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include "palette.hpp"

// PaletteShare packets travel on the game's own UDP socket, next to the game's packets.
//
//   [0]    u8   packet type 0xA7. The game handles types 1..14 only and ignores everything
//               else (0x41DB04), so peers without the mod drop these. giuroll claims
//               0x6B..0x80, SokuRoll 0x10..0x17, DesyncDetector 0x20..0x21, Soku2 0xD3.
//   [1..4] "PSHR" magic
//   [5]    u8   protocol version
//   [6]    u8   kind
//   [7..]  payload, little endian
//
// The game reads into a 1024-byte buffer and drops anything >= 1024 bytes, so palettes are sent
// in chunks of at most MAX_CHUNK bytes (a 16-bit palette fits in one, a 32-bit one needs two).

namespace ps::proto {
	constexpr uint8_t PACKET_TYPE = 0xA7;
	constexpr char MAGIC[4] = {'P', 'S', 'H', 'R'};
	constexpr uint8_t VERSION = 2;  // 2: palette chunks carry an offer sequence number
	constexpr size_t HEADER_SIZE = 7;
	constexpr size_t MAX_CHUNK = 900;

	enum Kind : uint8_t {
		KIND_HELLO = 1,   // u8 role (0 = player, 1 = spectator), u8 isReply
		KIND_PALETTE = 2, // u32 seq, u16 charId, u8 slot, u8[16] hash, u16 totalSize, u16 offset, u16 len, bytes
		KIND_ACK = 3,     // u8[16] hash
	};

	enum Role : uint8_t {
		ROLE_PLAYER = 0,
		ROLE_SPECTATOR = 1,
	};

	struct Hello {
		uint8_t version;
		Role role;
		bool isReply;
	};

	struct PaletteChunk {
		uint32_t seq;      // the sender's offer number this session: a higher one replaces a lower one
		uint16_t charId;
		uint8_t slot;
		PaletteHash hash;
		uint16_t totalSize;
		uint16_t offset;
		std::vector<uint8_t> data;
	};

	// True when the datagram is ours (type + magic), whatever its version or kind.
	bool isOurs(const uint8_t *data, size_t size);

	std::vector<uint8_t> makeHello(Role role, bool isReply);
	std::vector<uint8_t> makeAck(const PaletteHash &hash);
	// Splits the serialized palette into chunk packets.
	std::vector<std::vector<uint8_t>> makePalette(uint32_t seq, uint16_t charId, uint8_t slot, const PaletteHash &hash, const std::vector<uint8_t> &serialized);

	std::optional<Hello> parseHello(const uint8_t *data, size_t size);
	std::optional<PaletteChunk> parsePalette(const uint8_t *data, size_t size);
	std::optional<PaletteHash> parseAck(const uint8_t *data, size_t size);
	std::optional<uint8_t> kindOf(const uint8_t *data, size_t size);
	std::optional<uint8_t> versionOf(const uint8_t *data, size_t size);
}
