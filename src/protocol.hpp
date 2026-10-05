#pragma once
#include <cstdint>
#include <optional>
#include <string>
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
	constexpr uint8_t VERSION = 3;      // what we write; 3: palettes carry their side (and go to spectators)
	constexpr uint8_t MIN_VERSION = 3;  // the oldest version we can still talk (HELLO's min..max range)
	constexpr size_t HEADER_SIZE = 7;
	constexpr size_t MAX_CHUNK = 900;
	constexpr uint16_t MAX_CHARACTER_ID = 256;  // the game's are 0-20; room for modded characters

	enum Kind : uint8_t {
		KIND_HELLO = 1,   // u8 role (0 = player, 1 = spectator), u8 isReply, u8 min version, u8 max
		                  // version, u8 length + the mod's version string, then (a reply
		                  // to a spectator) u32 cookie. Both sides talk the highest version in both
		                  // ranges; HELLO's known fields never change, new ones are appended.
		KIND_PALETTE = 2, // u8 side (0 left, 1 right), u32 seq, u16 charId, u8 slot, u8[16] hash,
		                  // u16 totalSize, u16 offset, u16 len, bytes: what that side is shown with. To
		                  // the opponent: our own side's; to a spectator: either side's.
		KIND_ACK = 3,     // u8[16] hash, u32 seq: which palette this confirms (offers in a row often share
		                  // a palette while the wheel steps, so the hash alone could confirm the wrong one)
		KIND_JOIN = 4,    // spectator: u32 cookie from the HELLO reply. Proves the spectator gets our
		                  // packets at its address before any palette goes there (a forged sender
		                  // address never sees the cookie).
	};

	enum Role : uint8_t {
		ROLE_PLAYER = 0,
		ROLE_SPECTATOR = 1,
	};

	struct Hello {
		uint8_t version;     // the header's
		Role role;
		bool isReply;
		uint8_t minVersion;
		uint8_t maxVersion;
		std::string modVersion;
		uint32_t cookie = 0;     // a reply to a spectator: what its JOIN must carry (0 = none)
	};

	struct PaletteChunk {
		uint8_t side;
		uint32_t seq;      // the sender's number for it this session: a higher one replaces a lower one
		uint16_t charId;
		uint8_t slot;
		PaletteHash hash;
		uint16_t totalSize;
		uint16_t offset;
		std::vector<uint8_t> data;
	};

	// True when the datagram is ours (type + magic), whatever its version or kind.
	bool isOurs(const uint8_t *data, size_t size);

	std::vector<uint8_t> makeHello(Role role, bool isReply, uint32_t cookie = 0);
	std::vector<uint8_t> makeJoin(uint32_t cookie);
	std::optional<uint32_t> parseJoin(const uint8_t *data, size_t size);
	struct Ack {
		PaletteHash hash;
		uint32_t seq;
	};
	std::vector<uint8_t> makeAck(const PaletteHash &hash, uint32_t seq);
	// Splits the serialized palette into chunk packets.
	std::vector<std::vector<uint8_t>> makePalette(uint8_t side, uint32_t seq, uint16_t charId, uint8_t slot, const PaletteHash &hash, const std::vector<uint8_t> &serialized);

	std::optional<Hello> parseHello(const uint8_t *data, size_t size);
	std::optional<PaletteChunk> parsePalette(const uint8_t *data, size_t size);
	std::optional<Ack> parseAck(const uint8_t *data, size_t size);
	std::optional<uint8_t> kindOf(const uint8_t *data, size_t size);
	std::optional<uint8_t> versionOf(const uint8_t *data, size_t size);
}
