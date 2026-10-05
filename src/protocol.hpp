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
	constexpr uint8_t VERSION = 2;      // what we write; 2: palette chunks carry an offer sequence number
	constexpr uint8_t MIN_VERSION = 2;  // the oldest version we can still talk (HELLO's min..max range)
	constexpr size_t HEADER_SIZE = 7;
	constexpr size_t MAX_CHUNK = 900;
	constexpr uint16_t MAX_CHARACTER_ID = 256;  // the game's are 0-20; room for modded characters

	enum Kind : uint8_t {
		KIND_HELLO = 1,   // u8 role (0 = player, 1 = spectator), u8 isReply, then (since 1.0.0) u8 min
		                  // version, u8 max version, u8 length + the mod's version string. Both sides
		                  // talk the highest version in both ranges; HELLO's own layout never changes.
		KIND_PALETTE = 2, // u32 seq, u16 charId, u8 slot, u8[16] hash, u16 totalSize, u16 offset, u16 len, bytes
		KIND_ACK = 3,     // u8[16] hash, u32 seq: which offer this confirms (offers in a row often share
		                  // a palette while the wheel steps, so the hash alone could confirm the wrong one)
		KIND_SIDE = 4,    // to a spectator: u8 side (0 left, 1 right), then PALETTE's payload: what that
		                  // side's player is shown with; acknowledged with ACK. Test builds without it
		                  // ignore unknown kinds (and spectator HELLOs), so it needs no new version.
	};

	enum Role : uint8_t {
		ROLE_PLAYER = 0,
		ROLE_SPECTATOR = 1,
	};

	struct Hello {
		uint8_t version;     // the header's
		Role role;
		bool isReply;
		uint8_t minVersion;  // = version for peers that don't send a range
		uint8_t maxVersion;
		std::string modVersion;  // empty when not sent
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
	struct Ack {
		PaletteHash hash;
		uint32_t seq;  // 0 when the sender didn't say (never matches an offer)
	};
	std::vector<uint8_t> makeAck(const PaletteHash &hash, uint32_t seq);
	// Splits the serialized palette into chunk packets.
	std::vector<std::vector<uint8_t>> makePalette(uint32_t seq, uint16_t charId, uint8_t slot, const PaletteHash &hash, const std::vector<uint8_t> &serialized);
	// The same for a spectator, with the side.
	std::vector<std::vector<uint8_t>> makeSide(uint8_t side, uint32_t seq, uint16_t charId, uint8_t slot, const PaletteHash &hash, const std::vector<uint8_t> &serialized);
	struct SideChunk {
		uint8_t side;
		PaletteChunk chunk;
	};

	std::optional<Hello> parseHello(const uint8_t *data, size_t size);
	std::optional<PaletteChunk> parsePalette(const uint8_t *data, size_t size);
	std::optional<SideChunk> parseSide(const uint8_t *data, size_t size);
	std::optional<Ack> parseAck(const uint8_t *data, size_t size);
	std::optional<uint8_t> kindOf(const uint8_t *data, size_t size);
	std::optional<uint8_t> versionOf(const uint8_t *data, size_t size);
}
