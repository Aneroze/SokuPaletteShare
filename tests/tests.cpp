// Tests for the parts of PaletteShare that don't need the game: hashing, base64, palettes, the
// network protocol, the replay metadata format and the palette file readers, plus a fuzz pass that
// feeds mutated inputs to every parser (they must reject or accept, never crash or throw).
//
//   tools\test.bat        (builds the PaletteShareTests target and runs it)
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>
#include "../src/backup.hpp"
#include "../src/base64.hpp"
#include "../src/metaformat.hpp"
#include "../src/palette.hpp"
#include "../src/palettefile.hpp"
#include "../src/protocol.hpp"
#include "../src/sha256.hpp"

namespace {
	int g_failed = 0, g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

	using namespace ps;

	// Deterministic random numbers, so a failure can be reproduced.
	struct Rng {
		uint32_t s;
		uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
		uint32_t below(uint32_t n) { return n ? next() % n : 0; }
	};

	std::vector<uint8_t> mutate(std::vector<uint8_t> d, Rng &rng)
	{
		int edits = 1 + rng.below(4);
		for (int e = 0; e < edits; e++) {
			switch (rng.below(4)) {
			case 0: if (!d.empty()) d[rng.below(static_cast<uint32_t>(d.size()))] ^= static_cast<uint8_t>(1 << rng.below(8)); break;
			case 1: if (!d.empty()) d[rng.below(static_cast<uint32_t>(d.size()))] = static_cast<uint8_t>(rng.next()); break;
			case 2: if (!d.empty()) d.resize(rng.below(static_cast<uint32_t>(d.size()))); break;
			case 3: d.push_back(static_cast<uint8_t>(rng.next())); break;
			}
		}
		return d;
	}

	Palette testPalette(uint8_t bpp, uint8_t seed)
	{
		Palette p;
		p.bpp = bpp;
		p.colors.resize(Palette::colorBytes(bpp));
		for (size_t i = 0; i < p.colors.size(); i++)
			p.colors[i] = static_cast<uint8_t>(i * 7 + seed);
		return p;
	}

	std::filesystem::path g_dir;

	std::filesystem::path writeFile(const std::string &name, const std::vector<uint8_t> &data)
	{
		auto path = g_dir / name;
		std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(data.data()), data.size());
		return path;
	}

	std::vector<uint8_t> bytes(const std::string &s) { return {s.begin(), s.end()}; }

	void put16(std::vector<uint8_t> &d, uint32_t v) { d.push_back(v & 0xFF); d.push_back(v >> 8 & 0xFF); }
	void put32(std::vector<uint8_t> &d, uint32_t v) { put16(d, v & 0xFFFF); put16(d, v >> 16); }

	// A minimal .rep: header (input count at 0x77) and `inputs` 2-byte inputs.
	std::vector<uint8_t> fakeReplay(uint32_t inputs)
	{
		std::vector<uint8_t> d(0x7B + inputs * 2, 0x11);
		memcpy(&d[0x77], &inputs, 4);
		return d;
	}

	// An uncompressed BMP: 8-bit with a color table, or 24-bit pixels.
	std::vector<uint8_t> bmp(int bits, int width, int height)
	{
		std::vector<uint8_t> d;
		int table = bits == 8 ? 256 * 4 : 0;
		int stride = (width * bits + 31) / 32 * 4;
		d.push_back('B'); d.push_back('M');
		put32(d, 54 + table + stride * height);
		put32(d, 0);
		put32(d, 54 + table);
		put32(d, 40); put32(d, width); put32(d, height);
		put16(d, 1); put16(d, bits);
		put32(d, 0); put32(d, stride * height); put32(d, 0); put32(d, 0); put32(d, bits == 8 ? 256 : 0); put32(d, 0);
		for (int i = 0; i < table / 4; i++) {
			d.push_back(static_cast<uint8_t>(i)); d.push_back(static_cast<uint8_t>(255 - i)); d.push_back(static_cast<uint8_t>(i / 2)); d.push_back(0);
		}
		for (int i = 0; i < stride * height; i++)
			d.push_back(static_cast<uint8_t>(i * 3));
		return d;
	}

	void testSha256()
	{
		CHECK(sha256SelfTest());
		// Every length around the 55/56/64-byte padding edges, checked against a second computation
		// done in two halves (same input, so the digests must match).
		for (size_t n = 0; n < 200; n++) {
			std::string s(n, 'x');
			auto a = sha256(reinterpret_cast<const uint8_t *>(s.data()), s.size());
			auto b = sha256(reinterpret_cast<const uint8_t *>(s.data()), s.size());
			CHECK(a == b);
		}
	}

	void testBase64()
	{
		for (size_t n = 0; n < 40; n++) {
			std::vector<uint8_t> d(n);
			for (size_t i = 0; i < n; i++)
				d[i] = static_cast<uint8_t>(i * 37 + 5);
			auto text = base64::encode(d);
			CHECK(text.size() == (n + 2) / 3 * 4);
			auto back = base64::decode(text);
			CHECK(back && *back == d);
		}
		CHECK(base64::encode(bytes("Man")) == "TWFu");
		CHECK(!base64::decode("TW!u"));
	}

	void testPalettes()
	{
		for (uint8_t bpp : {16, 32}) {
			auto p = testPalette(bpp, 3);
			auto data = p.serialize();
			CHECK(data.size() == 1 + Palette::colorBytes(bpp));
			auto back = Palette::deserialize(data.data(), data.size());
			CHECK(back && back->colors == p.colors && back->bpp == bpp);
			CHECK(hashPalette(*back) == hashPalette(p));
			data.push_back(0);
			CHECK(!Palette::deserialize(data.data(), data.size()));
		}
		CHECK(!(hashPalette(testPalette(16, 1)) == hashPalette(testPalette(16, 2))));
		auto hex = hashPalette(testPalette(16, 1)).hex();
		CHECK(hex.size() == 32 && PaletteHash::fromHex(hex) && *PaletteHash::fromHex(hex) == hashPalette(testPalette(16, 1)));
		CHECK(!PaletteHash::fromHex("xyz"));

		// lockTransparency: the result keeps the custom colors but the reference's transparency.
		auto custom = testPalette(16, 9), reference = testPalette(16, 4);
		auto locked = lockTransparency(custom, reference);
		for (int i = 0; i < 256; i++) {
			CHECK((locked.colors[i * 2 + 1] & 0x80) == (reference.colors[i * 2 + 1] & 0x80));
			CHECK((locked.colors[i * 2 + 1] & 0x7F) == (custom.colors[i * 2 + 1] & 0x7F));
		}
		CHECK(sameColors(custom, locked));

		// withColorOf: one color taken over, across depths both ways.
		auto p16 = testPalette(16, 1), p32 = testPalette(32, 2);
		auto a = withColorOf(p16, p32, 31);
		auto expect = withColorOf(testPalette(16, 99), p32, 31);
		CHECK(memcmp(&a.colors[62], &expect.colors[62], 2) == 0);
		CHECK(memcmp(&a.colors[0], &p16.colors[0], 62) == 0 && memcmp(&a.colors[64], &p16.colors[64], 512 - 64) == 0);
		auto b = withColorOf(p32, p16, 31);
		CHECK(memcmp(&b.colors[0], &p32.colors[0], 124) == 0);
		Palette back16 = withColorOf(p16, b, 31);
		CHECK(memcmp(&back16.colors[62], &p16.colors[62], 2) == 0);  // 16 -> 32 -> 16 is exact
	}

	void testProtocol()
	{
		auto hello = proto::makeHello(proto::ROLE_PLAYER, true);
		CHECK(proto::isOurs(hello.data(), hello.size()));
		auto parsed = proto::parseHello(hello.data(), hello.size());
		CHECK(parsed && parsed->isReply && parsed->role == proto::ROLE_PLAYER);
		CHECK(parsed && parsed->minVersion == proto::MIN_VERSION && parsed->maxVersion == proto::VERSION && parsed->modVersion == PALETTESHARE_VERSION);
		std::vector<uint8_t> noRange(hello.begin(), hello.begin() + proto::HEADER_SIZE + 2);
		CHECK(!proto::parseHello(noRange.data(), noRange.size()));  // test builds before 1.0.0

		for (uint8_t bpp : {16, 32}) {
			auto p = testPalette(bpp, 5);
			auto hash = hashPalette(p);
			auto packets = proto::makePalette(42, 7, 3, hash, p.serialize());
			CHECK(packets.size() == (bpp == 16 ? 1u : 2u));
			std::vector<uint8_t> whole(1 + Palette::colorBytes(bpp));
			for (auto &packet : packets) {
				CHECK(packet.size() < 1024);  // the game drops datagrams of 1024 bytes and more
				auto chunk = proto::parsePalette(packet.data(), packet.size());
				CHECK(chunk && chunk->seq == 42 && chunk->charId == 7 && chunk->slot == 3 && chunk->hash == hash);
				if (chunk)
					memcpy(whole.data() + chunk->offset, chunk->data.data(), chunk->data.size());
			}
			CHECK(whole == p.serialize());
		}
		auto p = testPalette(16, 1);
		CHECK(proto::parsePalette(proto::makePalette(1, 300, 0, hashPalette(p), p.serialize())[0].data(), 30) == std::nullopt);
		auto badChar = proto::makePalette(1, 300, 0, hashPalette(p), p.serialize())[0];
		CHECK(!proto::parsePalette(badChar.data(), badChar.size()));  // charId past 255
		auto badSlot = proto::makePalette(1, 3, 9, hashPalette(p), p.serialize())[0];
		CHECK(!proto::parsePalette(badSlot.data(), badSlot.size()));  // slot past 7
		auto ack = proto::makeAck(hashPalette(p), 77);
		auto parsedAck = proto::parseAck(ack.data(), ack.size());
		CHECK(parsedAck && parsedAck->hash == hashPalette(p) && parsedAck->seq == 77);
		std::vector<uint8_t> oldAck(ack.begin(), ack.end() - 4);  // without the offer number
		CHECK(!proto::parseAck(oldAck.data(), oldAck.size()));
		// Spectators: a side's palette.
		for (uint8_t side : {0, 1}) {
			auto p32 = testPalette(32, 6);
			auto sidePackets = proto::makeSide(side, 9, 10, 4, hashPalette(p32), p32.serialize());
			CHECK(sidePackets.size() == 2);
			for (auto &packet : sidePackets) {
				CHECK(packet.size() < 1024);
				auto parsed = proto::parseSide(packet.data(), packet.size());
				CHECK(parsed && parsed->side == side && parsed->chunk.seq == 9 && parsed->chunk.charId == 10 && parsed->chunk.slot == 4);
				CHECK(!proto::parsePalette(packet.data(), packet.size()));  // not mistaken for the opponent's
			}
		}
		// The spectator handshake: the cookie rides on the HELLO reply, JOIN echoes it.
		auto withCookie = proto::makeHello(proto::ROLE_PLAYER, true, 0xC0FFEE42);
		auto parsedCookie = proto::parseHello(withCookie.data(), withCookie.size());
		CHECK(parsedCookie && parsedCookie->isReply && parsedCookie->cookie == 0xC0FFEE42 && !parsedCookie->modVersion.empty());
		auto plainHello = proto::makeHello(proto::ROLE_SPECTATOR, false);
		auto parsedPlain = proto::parseHello(plainHello.data(), plainHello.size());
		CHECK(parsedPlain && parsedPlain->cookie == 0 && parsedPlain->role == proto::ROLE_SPECTATOR);
		auto join = proto::makeJoin(0xC0FFEE42);
		CHECK(proto::parseJoin(join.data(), join.size()) == 0xC0FFEE42u);
		auto zeroJoin = proto::makeJoin(0);
		CHECK(!proto::parseJoin(zeroJoin.data(), zeroJoin.size()));  // 0 is "no cookie"
		CHECK(!proto::parseJoin(join.data(), join.size() - 1));
		CHECK(!proto::parseSide(join.data(), join.size()) && !proto::parseAck(join.data(), join.size()));
		auto badSide = proto::makeSide(2, 1, 3, 0, hashPalette(p), p.serialize())[0];
		CHECK(!proto::parseSide(badSide.data(), badSide.size()));  // only sides 0 and 1
		std::vector<uint8_t> notOurs = {0x05, 'P', 'S', 'H', 'R', 2, 1};
		CHECK(!proto::isOurs(notOurs.data(), notOurs.size()));
	}

	void testMetaFormat()
	{
		using namespace meta::format;
		std::string deep(MAX_DEPTH, '['), deeper(MAX_DEPTH + 1, '[');
		CHECK(shallowEnough(deep + std::string(MAX_DEPTH, ']')));
		CHECK(!shallowEnough(deeper + std::string(MAX_DEPTH + 1, ']')));
		CHECK(shallowEnough("{\"a\":\"" + std::string(100, '[') + "\"}"));  // brackets inside a string don't count
		CHECK(shallowEnough("{\"a\":\"\\\"" + std::string(100, '[') + "\"}"));  // nor after an escaped quote
		CHECK(parseObject("{\"a\":1}") && !parseObject("[1]") && !parseObject("{") && !parseObject(deeper));

		auto replay = fakeReplay(100);
		auto end = gameDataEnd(replay.data(), replay.size(), replay.size());
		CHECK(end && *end == replay.size());
		CHECK(!gameDataEnd(replay.data(), 10, replay.size()));
		auto short_ = fakeReplay(100);
		short_.resize(short_.size() - 1);
		CHECK(!gameDataEnd(short_.data(), short_.size(), short_.size()));
	}

	void testPaletteFiles()
	{
		const char *why = nullptr;
		auto game16 = testPalette(16, 2).serialize();
		auto p = readPaletteFile(writeFile("game.pal", game16), &why);
		CHECK(p && p->bpp == 16);
		auto shady = game16;
		shady.push_back(0); shady.push_back(0);  // shady-loader packs ship some as 515 bytes
		CHECK(readPaletteFile(writeFile("shady.pal", shady), &why));

		std::string jasc = "JASC-PAL\r\n0100\r\n256\r\n";
		for (int i = 0; i < 256; i++)
			jasc += std::to_string(i) + " " + std::to_string(255 - i) + " 7\r\n";
		auto j = readPaletteFile(writeFile("jasc.pal", bytes(jasc)), &why);
		CHECK(j && j->bpp == 32 && j->colors[4 + 2] == 1 && j->colors[4 + 1] == 254);

		std::vector<uint8_t> riff = bytes("RIFF");
		put32(riff, 4 + 8 + 4 + 256 * 4);
		riff.insert(riff.end(), {'P', 'A', 'L', ' ', 'd', 'a', 't', 'a'});
		put32(riff, 4 + 256 * 4);
		put16(riff, 0x0300); put16(riff, 256);
		for (int i = 0; i < 256; i++) { riff.push_back(static_cast<uint8_t>(i)); riff.push_back(3); riff.push_back(4); riff.push_back(0); }
		auto r = readPaletteFile(writeFile("riff.pal", riff), &why);
		CHECK(r && r->colors[5 * 4 + 2] == 5);

		std::vector<uint8_t> editor(1024 + 20, 0x40);  // the old Palette Editor: 0x00RRGGBB, then a name
		CHECK(readPaletteFile(writeFile("editor.pal", editor), &why));

		std::vector<uint8_t> act(768);
		for (int i = 0; i < 768; i++) act[i] = static_cast<uint8_t>(i);
		CHECK(readPaletteFile(writeFile("plain.act", act), &why));
		act.push_back(0); act.push_back(16); act.push_back(0xFF); act.push_back(0xFF);  // count 16
		auto a = readPaletteFile(writeFile("counted.act", act), &why);
		CHECK(a && a->colors[20 * 4 + 2] == 0);  // colors past the count stay black

		auto b8 = readPaletteFile(writeFile("indexed.bmp", bmp(8, 16, 16)), &why);
		CHECK(b8 && b8->colors[9 * 4 + 2] == 9 / 2 && b8->colors[9 * 4 + 1] == 255 - 9);
		CHECK(readPaletteFile(writeFile("swatch.bmp", bmp(24, 16, 16)), &why));
		CHECK(!readPaletteFile(writeFile("tiny.bmp", bmp(24, 4, 4)), &why));  // fewer than 256 pixels

		CHECK(!readPaletteFile(writeFile("short.pal", std::vector<uint8_t>(100, 1)), &why) && why);
		CHECK(!readPaletteFile(writeFile("huge.pal", std::vector<uint8_t>(200 * 1024, 1)), &why));  // over the 64 KB cap
		CHECK(!readPaletteFile(g_dir / "missing.pal", &why));
	}

	// Mutated inputs: every parser either rejects them or returns something valid, never crashes or throws.
	void fuzz()
	{
		Rng rng{0x5EED1234};
		auto p16 = testPalette(16, 1), p32 = testPalette(32, 2);
		std::vector<std::vector<uint8_t>> packets = {proto::makeHello(proto::ROLE_PLAYER, false), proto::makeAck(hashPalette(p16), 5)};
		for (auto &chunk : proto::makePalette(9, 3, 1, hashPalette(p32), p32.serialize()))
			packets.push_back(chunk);
		for (auto &chunk : proto::makeSide(1, 9, 3, 1, hashPalette(p16), p16.serialize()))
			packets.push_back(chunk);
		packets.push_back(proto::makeJoin(0x12345678));
		packets.push_back(proto::makeHello(proto::ROLE_PLAYER, true, 0x12345678));
		int accepted = 0;
		for (int n = 0; n < 200000; n++) {
			auto d = mutate(packets[rng.below(static_cast<uint32_t>(packets.size()))], rng);
			try {
				if (auto c = proto::parsePalette(d.data(), d.size())) {
					accepted++;
					CHECK(c->offset + c->data.size() <= c->totalSize && c->slot <= 7 && c->charId < proto::MAX_CHARACTER_ID);
				}
				if (auto c = proto::parseSide(d.data(), d.size()))
					CHECK(c->side <= 1 && c->chunk.offset + c->chunk.data.size() <= c->chunk.totalSize && c->chunk.slot <= 7);
				proto::parseHello(d.data(), d.size());
				proto::parseAck(d.data(), d.size());
				proto::parseJoin(d.data(), d.size());
				Palette::deserialize(d.data(), d.size());
			} catch (...) {
				CHECK(!"a protocol parser threw");
			}
		}
		printf("  protocol: 200000 mutated packets, %d still valid palette chunks\n", accepted);

		std::string json = "{\"PaletteShare\":{\"version\":1,\"palettes\":{\"p1\":{\"character\":0,\"slot\":7,\"palette\":\"AAAA\"}}}}";
		for (int n = 0; n < 100000; n++) {
			auto d = mutate(bytes(json), rng);
			try {
				meta::format::parseObject(std::string(d.begin(), d.end()));
			} catch (...) {
				CHECK(!"the JSON reader threw");
			}
		}
		printf("  replay metadata: 100000 mutated documents\n");

		std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
			{"f.pal", testPalette(16, 3).serialize()}, {"f.act", std::vector<uint8_t>(772, 9)},
			{"f.bmp", bmp(8, 16, 16)}, {"g.bmp", bmp(24, 16, 16)}, {"j.pal", bytes("JASC-PAL\r\n0100\r\n2\r\n1 2 3\r\n4 5 6\r\n")},
		};
		for (int n = 0; n < 3000; n++) {
			auto &[name, data] = files[rng.below(static_cast<uint32_t>(files.size()))];
			try {
				if (auto read = readPaletteFile(writeFile(name, mutate(data, rng))))
					CHECK(read->valid());
			} catch (...) {
				CHECK(!"a palette file reader threw");
			}
		}
		printf("  palette files: 3000 mutated files\n");
	}
	// The palette backup (backup.hpp), in folders of its own under the test folder.
	void testBackup()
	{
		namespace fs = std::filesystem;
		auto root = g_dir / L"backup";
		std::error_code ec;
		fs::remove_all(root, ec);
		auto lib = root / L"modules" / L"PaletteShare" / L"palettes", bak = root / L"PaletteShare" / L"backup";
		auto put = [](const fs::path &file, const std::string &text, int ageSeconds = 0) {
			fs::create_directories(file.parent_path());
			{ std::ofstream(file, std::ios::binary) << text; }
			if (ageSeconds)
				fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::seconds(ageSeconds));
		};
		auto read = [](const fs::path &file) {
			std::ifstream in(file, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		};
		auto has = [](const fs::path &file) { std::error_code e; return fs::exists(file, e); };

		// A first start: everything gets backed up.
		put(lib / L"reimu" / L"Blue.pal", "blue");
		put(lib / L"reimu" / L"palettes.ini", "settings");
		put(lib / L"reimu" / L"Mine.pal", "mine");
		backup::start(lib, bak);
		CHECK(read(bak / L"reimu" / L"Blue.pal") == "blue" && read(bak / L"reimu" / L"palettes.ini") == "settings");
		CHECK(read(bak / L"reimu" / L"Mine.pal") == "mine");
		CHECK(has(lib / backup::MARKER) && has(bak / L"README.txt"));

		// Added, changed and deleted palettes.
		put(lib / L"reimu" / L"Green.pal", "green");
		put(lib / L"reimu" / L"Mine.pal", "mine, edited");
		fs::remove(lib / L"reimu" / L"Blue.pal");
		backup::character(L"reimu");
		CHECK(read(bak / L"reimu" / L"Green.pal") == "green");
		CHECK(read(bak / L"reimu" / L"Mine.pal") == "mine, edited");
		CHECK(!has(bak / L"reimu" / L"Blue.pal") && read(bak / backup::REMOVED / L"reimu" / L"Blue.pal") == "blue");

		// A whole character folder deleted while the game was closed; an update (marker kept)
		// doesn't bring anything back.
		put(lib / L"marisa" / L"Star.pal", "star");
		backup::character(L"marisa");
		fs::remove_all(lib / L"marisa");
		backup::start(lib, bak);
		CHECK(!has(lib / L"marisa" / L"Star.pal") && !has(lib / L"reimu" / L"Blue.pal"));
		CHECK(read(bak / backup::REMOVED / L"marisa" / L"Star.pal") == "star");

		// Removed and installed again: everything comes back, the zip's (newer) files stay.
		fs::remove_all(root / L"modules");
		put(lib / L"reimu" / L"Mine.pal", "from the zip");
		backup::start(lib, bak);
		CHECK(read(lib / L"reimu" / L"Green.pal") == "green");
		CHECK(read(lib / L"reimu" / L"palettes.ini") == "settings");
		CHECK(read(lib / L"reimu" / L"Mine.pal") == "from the zip");
		CHECK(!has(lib / L"reimu" / L"Blue.pal") && !has(lib / L"marisa" / L"Star.pal"));
		CHECK(has(lib / backup::MARKER));

		// Deleted twice under the same name: both kept in _removed.
		put(lib / L"reimu" / L"Twice.pal", "first");
		backup::character(L"reimu");
		fs::remove(lib / L"reimu" / L"Twice.pal");
		backup::character(L"reimu");
		put(lib / L"reimu" / L"Twice.pal", "second");
		backup::character(L"reimu");
		fs::remove(lib / L"reimu" / L"Twice.pal");
		backup::character(L"reimu");
		CHECK(read(bak / backup::REMOVED / L"reimu" / L"Twice.pal") == "first");
		CHECK(read(bak / backup::REMOVED / L"reimu" / L"Twice (2).pal") == "second");

		// A read-only palette: its backup copy doesn't stay read-only, so later edits still get there.
		put(lib / L"reimu" / L"Locked.pal", "v1");
		SetFileAttributesW((lib / L"reimu" / L"Locked.pal").c_str(), FILE_ATTRIBUTE_READONLY);
		backup::character(L"reimu");
		CHECK(!(GetFileAttributesW((bak / L"reimu" / L"Locked.pal").c_str()) & FILE_ATTRIBUTE_READONLY));
		SetFileAttributesW((lib / L"reimu" / L"Locked.pal").c_str(), FILE_ATTRIBUTE_NORMAL);
		put(lib / L"reimu" / L"Locked.pal", "v2");
		backup::character(L"reimu");
		CHECK(read(bak / L"reimu" / L"Locked.pal") == "v2");

		// A reinstall where a palette can't be copied back (here a folder in its way): no marker (the
		// next start tries again), and the backup keeps it.
		fs::remove_all(root / L"modules");
		fs::create_directories(lib / L"reimu" / L"Green.pal");  // a folder with the palette's name
		backup::start(lib, bak);
		CHECK(!has(lib / backup::MARKER));
		CHECK(read(bak / L"reimu" / L"Green.pal") == "green" && !has(bak / backup::REMOVED / L"reimu" / L"Green.pal"));
		backup::character(L"reimu");  // also not later in this run
		CHECK(read(bak / L"reimu" / L"Green.pal") == "green");
		fs::remove_all(lib / L"reimu" / L"Green.pal");
		backup::start(lib, bak);  // next start: it comes back
		CHECK(read(lib / L"reimu" / L"Green.pal") == "green" && has(lib / backup::MARKER));

		// No backup (game folder not writable): the library still works and nothing is written.
		fs::remove_all(root, ec);
		put(lib / L"reimu" / L"Mine.pal", "mine");
		backup::start(lib, fs::path());
		CHECK(has(lib / backup::MARKER) && !has(bak));
		backup::character(L"reimu");
		CHECK(!has(bak));
		fs::remove_all(root, ec);
	}
}

int main()
{
	wchar_t temp[MAX_PATH];
	GetTempPathW(MAX_PATH, temp);
	g_dir = std::filesystem::path(temp) / L"PaletteShareTests";
	std::filesystem::create_directories(g_dir);

	struct { const char *name; void (*run)(); } suites[] = {
		{"sha256", testSha256}, {"base64", testBase64}, {"palettes", testPalettes}, {"protocol", testProtocol},
		{"replay metadata", testMetaFormat}, {"palette files", testPaletteFiles}, {"backup", testBackup}, {"fuzz", fuzz},
	};
	for (auto &suite : suites) {
		printf("%s\n", suite.name);
		suite.run();
	}
	std::error_code ec;
	std::filesystem::remove_all(g_dir, ec);
	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
