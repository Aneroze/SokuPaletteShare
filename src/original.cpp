#include "original.hpp"
#include "log.hpp"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

// Archive format (Hisoutensoku/Hisouten .dat): u16 file count, u32 list size, then the file list,
// encrypted twice: XOR with MT19937 (seeded with list size + 6) output bytes, then a rolling XOR
// (k = 0xC5, k += t, t starts at 0x83 and grows by 0x53). Each entry: u32 offset, u32 size, u8 name
// length, name. A file's bytes are XORed with ((offset >> 1) | 0x23). Later archives override
// earlier ones, in the game's load order.
//
// The archives are the player's own game files, but they're parsed like untrusted input anyway:
// sizes are capped and every offset is checked against the file size.
namespace ps::original {
	namespace {
		const char *const ARCHIVES[] = {"th105a.dat", "th105b.dat", "th123a.dat", "th123b.dat", "th123c.dat"};
		constexpr uint32_t MAX_LIST_SIZE = 16 * 1024 * 1024;
		constexpr uint32_t MAX_PALETTE_SIZE = 4096;  // the game's are 513 bytes (1025 for 32-bit)

		struct Entry {
			int archive;
			uint32_t offset;
			uint32_t size;
		};

		std::mutex g_mutex;
		bool g_indexed = false;
		std::filesystem::path g_dir;
		std::map<std::string, Entry> g_entries;            // "data/character/<name>/paletteNNN.pal"
		std::map<std::string, std::optional<Palette>> g_cache;

		std::filesystem::path gameDir()
		{
			wchar_t exe[MAX_PATH];
			DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
			return n && n < MAX_PATH ? std::filesystem::path(exe).parent_path() : std::filesystem::path();
		}

		uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

		void indexArchive(int archive)
		{
			auto path = g_dir / ARCHIVES[archive];
			std::ifstream in(path, std::ios::binary);
			if (!in)
				return;
			std::error_code ec;
			uint64_t fileSize = std::filesystem::file_size(path, ec);
			uint8_t header[6];
			if (ec || !in.read(reinterpret_cast<char *>(header), sizeof(header)))
				return;
			uint32_t count = header[0] | header[1] << 8;
			uint32_t listSize = u32(header + 2);
			if (listSize > MAX_LIST_SIZE || 6 + uint64_t(listSize) > fileSize) {
				log("original: %s isn't a game archive, skipped", ARCHIVES[archive]);
				return;
			}
			std::vector<uint8_t> list(listSize);
			if (!in.read(reinterpret_cast<char *>(list.data()), listSize))
				return;

			std::mt19937 mt(listSize + 6);
			for (auto &b : list)
				b ^= static_cast<uint8_t>(mt());
			uint8_t k = 0xC5, t = 0x83;
			for (auto &b : list) {
				b ^= k;
				k += t;
				t += 0x53;
			}

			size_t at = 0, found = 0;
			for (uint32_t i = 0; i < count; i++) {
				if (list.size() - at < 9)
					break;
				uint32_t offset = u32(&list[at]), size = u32(&list[at + 4]);
				size_t nameLength = list[at + 8];
				at += 9;
				if (list.size() - at < nameLength)
					break;
				std::string name(reinterpret_cast<const char *>(&list[at]), nameLength);
				at += nameLength;
				for (auto &c : name)
					c = c == '\\' ? '/' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
				if (name.rfind("data/character/", 0) != 0 || name.find("/palette") == std::string::npos)
					continue;
				if (size > MAX_PALETTE_SIZE || uint64_t(offset) + size > fileSize)
					continue;
				g_entries[name] = {archive, offset, size};
				found++;
			}
			log("original: %s has %d character palette(s)", ARCHIVES[archive], (int)found);
		}

		void buildIndex()
		{
			g_indexed = true;
			g_dir = gameDir();
			for (int i = 0; i < static_cast<int>(std::size(ARCHIVES)); i++)
				indexArchive(i);
		}

		std::optional<Palette> read(const Entry &e)
		{
			std::ifstream in(g_dir / ARCHIVES[e.archive], std::ios::binary);
			std::vector<uint8_t> data(e.size);
			if (!in.seekg(e.offset) || !in.read(reinterpret_cast<char *>(data.data()), e.size))
				return std::nullopt;
			uint8_t key = static_cast<uint8_t>((e.offset >> 1) | 0x23);
			for (auto &b : data)
				b ^= key;
			return Palette::deserialize(data.data(), data.size());
		}
	}

	std::optional<Palette> palette(const char *charName, int slot)
	{
		if (!charName || !*charName || slot < 0 || slot > 7)
			return std::nullopt;
		char name[96];
		snprintf(name, sizeof(name), "data/character/%s/palette%03d.pal", charName, slot);
		for (char *c = name; *c; c++)
			*c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));

		std::lock_guard lock(g_mutex);
		if (!g_indexed)
			buildIndex();
		auto cached = g_cache.find(name);
		if (cached != g_cache.end())
			return cached->second;
		auto entry = g_entries.find(name);
		auto result = entry == g_entries.end() ? std::nullopt : read(entry->second);
		if (!result)
			log("original: no readable %s in the archives", name);
		return g_cache[name] = result;
	}
}
