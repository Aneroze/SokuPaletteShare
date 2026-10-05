#include "protocol.hpp"
#include <algorithm>
#include <cstring>

namespace ps::proto {
	namespace {
		class Writer {
		public:
			explicit Writer(Kind kind)
			{
				u8(PACKET_TYPE);
				bytes(reinterpret_cast<const uint8_t *>(MAGIC), 4);
				u8(VERSION);
				u8(kind);
			}

			void u8(uint8_t v) { _out.push_back(v); }
			void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
			void u32(uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); }
			void bytes(const uint8_t *p, size_t n) { _out.insert(_out.end(), p, p + n); }
			std::vector<uint8_t> take() { return std::move(_out); }

		private:
			std::vector<uint8_t> _out;
		};

		class Reader {
		public:
			Reader(const uint8_t *data, size_t size) : _data(data), _size(size), _pos(HEADER_SIZE) {}

			bool u8(uint8_t &v) { if (_pos + 1 > _size) return false; v = _data[_pos++]; return true; }
			bool u16(uint16_t &v)
			{
				if (_pos + 2 > _size) return false;
				v = static_cast<uint16_t>(_data[_pos] | _data[_pos + 1] << 8);
				_pos += 2;
				return true;
			}
			bool u32(uint32_t &v)
			{
				uint16_t lo, hi;
				if (!u16(lo) || !u16(hi)) return false;
				v = lo | static_cast<uint32_t>(hi) << 16;
				return true;
			}
			bool bytes(uint8_t *out, size_t n) { if (_pos + n > _size) return false; memcpy(out, _data + _pos, n); _pos += n; return true; }
			bool atEnd() const { return _pos == _size; }

		private:
			const uint8_t *_data;
			size_t _size;
			size_t _pos;
		};
	}

	bool isOurs(const uint8_t *data, size_t size)
	{
		return size >= HEADER_SIZE && data[0] == PACKET_TYPE && memcmp(data + 1, MAGIC, 4) == 0;
	}

	std::optional<uint8_t> versionOf(const uint8_t *data, size_t size)
	{
		if (!isOurs(data, size))
			return std::nullopt;
		return data[5];
	}

	std::optional<uint8_t> kindOf(const uint8_t *data, size_t size)
	{
		if (!isOurs(data, size))
			return std::nullopt;
		return data[6];
	}

	std::vector<uint8_t> makeHello(Role role, bool isReply, uint32_t cookie)
	{
		Writer w(KIND_HELLO);
		w.u8(role);
		w.u8(isReply ? 1 : 0);
		w.u8(MIN_VERSION);
		w.u8(VERSION);
		const char *mod = PALETTESHARE_VERSION;
		w.u8(static_cast<uint8_t>(strlen(mod)));
		w.bytes(reinterpret_cast<const uint8_t *>(mod), strlen(mod));
		if (cookie)
			w.u32(cookie);
		return w.take();
	}

	std::vector<uint8_t> makeJoin(uint32_t cookie)
	{
		Writer w(KIND_JOIN);
		w.u32(cookie);
		return w.take();
	}

	std::optional<uint32_t> parseJoin(const uint8_t *data, size_t size)
	{
		if (kindOf(data, size) != KIND_JOIN)
			return std::nullopt;
		Reader r(data, size);
		uint32_t cookie;
		if (!r.u32(cookie) || !cookie)
			return std::nullopt;
		return cookie;
	}

	std::vector<uint8_t> makeAck(const PaletteHash &hash, uint32_t seq)
	{
		Writer w(KIND_ACK);
		w.bytes(hash.bytes.data(), hash.bytes.size());
		w.u32(seq);
		return w.take();
	}

	std::vector<std::vector<uint8_t>> makePalette(uint8_t side, uint32_t seq, uint16_t charId, uint8_t slot, const PaletteHash &hash, const std::vector<uint8_t> &serialized)
	{
		std::vector<std::vector<uint8_t>> packets;
		for (size_t offset = 0; offset < serialized.size(); offset += MAX_CHUNK) {
			size_t len = std::min(MAX_CHUNK, serialized.size() - offset);
			Writer w(KIND_PALETTE);
			w.u8(side);
			w.u32(seq);
			w.u16(charId);
			w.u8(slot);
			w.bytes(hash.bytes.data(), hash.bytes.size());
			w.u16(static_cast<uint16_t>(serialized.size()));
			w.u16(static_cast<uint16_t>(offset));
			w.u16(static_cast<uint16_t>(len));
			w.bytes(serialized.data() + offset, len);
			packets.push_back(w.take());
		}
		return packets;
	}

	std::optional<Hello> parseHello(const uint8_t *data, size_t size)
	{
		if (kindOf(data, size) != KIND_HELLO)
			return std::nullopt;

		Reader r(data, size);
		uint8_t role, reply;
		if (!r.u8(role) || !r.u8(reply))
			return std::nullopt;
		// Newer versions may append fields; only the ones we know are read.
		Hello hello{data[5], static_cast<Role>(role), reply != 0, 0, 0, {}};
		uint8_t length;
		char mod[256];
		if (!r.u8(hello.minVersion) || !r.u8(hello.maxVersion) || hello.minVersion > hello.maxVersion ||
		    !r.u8(length) || !r.bytes(reinterpret_cast<uint8_t *>(mod), length))
			return std::nullopt;
		hello.modVersion.assign(mod, length);
		for (auto &c : hello.modVersion)
			if (c < 0x20 || c > 0x7E)
				c = '?';  // only for logs
		r.u32(hello.cookie);  // optional: stays 0
		return hello;
	}

	std::optional<PaletteChunk> parsePalette(const uint8_t *data, size_t size)
	{
		if (kindOf(data, size) != KIND_PALETTE)
			return std::nullopt;

		Reader r(data, size);
		PaletteChunk c;
		uint16_t len;
		if (!r.u8(c.side) || !r.u32(c.seq) || !r.u16(c.charId) || !r.u8(c.slot) || !r.bytes(c.hash.bytes.data(), c.hash.bytes.size()) ||
		    !r.u16(c.totalSize) || !r.u16(c.offset) || !r.u16(len))
			return std::nullopt;
		if (c.side > 1 || c.charId >= MAX_CHARACTER_ID || c.slot > 7)
			return std::nullopt;
		// Only the two real palette sizes, and a chunk that fits inside it.
		if (c.totalSize != 1 + Palette::colorBytes(16) && c.totalSize != 1 + Palette::colorBytes(32))
			return std::nullopt;
		if (len == 0 || len > MAX_CHUNK || c.offset + len > c.totalSize)
			return std::nullopt;
		c.data.resize(len);
		if (!r.bytes(c.data.data(), len) || !r.atEnd())
			return std::nullopt;
		return c;
	}

	std::optional<Ack> parseAck(const uint8_t *data, size_t size)
	{
		if (kindOf(data, size) != KIND_ACK)
			return std::nullopt;

		Reader r(data, size);
		Ack ack{{}, 0};
		if (!r.bytes(ack.hash.bytes.data(), ack.hash.bytes.size()) || !r.u32(ack.seq))
			return std::nullopt;
		return ack;
	}

}
