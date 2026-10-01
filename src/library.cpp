#include "library.hpp"
#include "log.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

namespace ps {
	namespace {
		std::filesystem::path g_root;
		std::mutex g_mutex;
		std::map<int, CharacterLibrary> g_cache;
		std::map<int, std::filesystem::file_time_type> g_stamp;

		std::vector<uint8_t> readAll(const std::filesystem::path &file)
		{
			std::ifstream in(file, std::ios::binary);
			return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}

		// 32-bit palette from 256 RGB colors: index 0 transparent, the rest opaque (the game's rule).
		Palette fromRgb(const uint8_t (&rgb)[256][3])
		{
			Palette p;
			p.bpp = 32;
			p.colors.resize(1024);
			for (int i = 0; i < 256; i++) {
				p.colors[i * 4 + 0] = rgb[i][2];  // B
				p.colors[i * 4 + 1] = rgb[i][1];  // G
				p.colors[i * 4 + 2] = rgb[i][0];  // R
				p.colors[i * 4 + 3] = i == 0 ? 0x00 : 0xFF;
			}
			return p;
		}

		std::optional<Palette> readBmp(const std::vector<uint8_t> &d)
		{
			auto u16 = [&](size_t o) { return o + 2 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8) : 0u; };
			auto u32 = [&](size_t o) { return o + 4 <= d.size() ? uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | d[o + 3] << 24) : 0u; };
			if (d.size() < 54 || d[0] != 'B' || d[1] != 'M')
				return std::nullopt;

			uint32_t dataOffset = u32(10);
			uint32_t headerSize = u32(14);
			int32_t width = static_cast<int32_t>(u32(18));
			int32_t height = static_cast<int32_t>(u32(22));
			uint32_t bits = u16(28);
			uint32_t compression = u32(30);
			uint32_t used = u32(46);
			if (headerSize < 40 || width <= 0 || height == 0 || compression != 0 && compression != 3)
				return std::nullopt;

			uint8_t rgb[256][3] = {};
			if (bits <= 8) {
				// Indexed image: its color table is the palette.
				uint32_t count = used ? std::min<uint32_t>(used, 256) : (1u << bits);
				size_t table = 14 + headerSize;
				if (table + count * 4 > d.size())
					return std::nullopt;
				for (uint32_t i = 0; i < count; i++) {
					rgb[i][0] = d[table + i * 4 + 2];
					rgb[i][1] = d[table + i * 4 + 1];
					rgb[i][2] = d[table + i * 4 + 0];
				}
				return fromRgb(rgb);
			}

			if (bits != 24 && bits != 32)
				return std::nullopt;
			// True-color swatch: the first 256 pixels, row by row from the top.
			size_t bpp = bits / 8;
			size_t stride = (static_cast<size_t>(width) * bits + 31) / 32 * 4;
			int rows = height < 0 ? -height : height;
			if (static_cast<size_t>(width) * rows < 256)
				return std::nullopt;
			for (int i = 0; i < 256; i++) {
				int x = i % width, y = i / width;
				int fileRow = height > 0 ? rows - 1 - y : y;
				size_t at = dataOffset + fileRow * stride + x * bpp;
				if (at + 3 > d.size())
					return std::nullopt;
				rgb[i][0] = d[at + 2];
				rgb[i][1] = d[at + 1];
				rgb[i][2] = d[at + 0];
			}
			return fromRgb(rgb);
		}

		std::wstring lower(std::wstring s)
		{
			std::transform(s.begin(), s.end(), s.begin(), towlower);
			return s;
		}

		std::wstring charFolderName(int charId)
		{
			auto getName = reinterpret_cast<const char *(*)(int)>(0x43F3F0);
			const char *name = getName(charId);
			if (!name || !*name)
				return L"";
			wchar_t wide[64];
			MultiByteToWideChar(CP_ACP, 0, name, -1, wide, 64);
			return wide;
		}

		CharacterLibrary load(int charId)
		{
			CharacterLibrary lib;
			lib.charId = charId;
			lib.folderName = charFolderName(charId);
			if (lib.folderName.empty())
				return lib;
			lib.folder = g_root / lib.folderName;
			std::error_code ec;
			std::filesystem::create_directories(lib.folder, ec);

			auto ini = (lib.folder / L"palettes.ini").wstring();
			lib.defaultFallback = std::clamp<int>(GetPrivateProfileIntW(L"default", L"fallback", 0, ini.c_str()), 0, 7);

			wchar_t buffer[256];
			GetPrivateProfileStringW(L"last", L"palette", L"", buffer, 256, ini.c_str());
			lib.lastSelected = buffer;
			lib.lastSlot = static_cast<int>(GetPrivateProfileIntW(L"last", L"slot", -1, ini.c_str()));

			for (auto &entry : std::filesystem::directory_iterator(lib.folder, ec)) {
				if (!entry.is_regular_file())
					continue;
				auto ext = lower(entry.path().extension().wstring());
				if (ext != L".pal" && ext != L".act" && ext != L".bmp")
					continue;
				auto palette = readPaletteFile(entry.path());
				if (!palette) {
					log("library: %ls is not a palette I can read, skipped", entry.path().filename().c_str());
					continue;
				}
				LibraryPalette item;
				item.name = entry.path().stem().wstring();
				item.file = entry.path();
				item.palette = std::move(*palette);
				item.hash = hashPalette(item.palette);
				int fallback = static_cast<int>(GetPrivateProfileIntW(item.name.c_str(), L"fallback", -1, ini.c_str()));
				item.fallbackIsDefault = fallback < 0 || fallback > 7;
				item.fallback = item.fallbackIsDefault ? lib.defaultFallback : fallback;
				lib.palettes.push_back(std::move(item));
			}
			std::sort(lib.palettes.begin(), lib.palettes.end(), [](auto &a, auto &b) { return lower(a.name) < lower(b.name); });
			log("library: %ls has %d palette(s), default fallback %d", lib.folderName.c_str(), (int)lib.palettes.size(), lib.defaultFallback);
			return lib;
		}

		std::filesystem::file_time_type stampOf(const std::filesystem::path &folder)
		{
			// The folder's own time changes when files are added/removed; palettes.ini and edited
			// files change their own times, so take the newest of all.
			std::error_code ec;
			auto newest = std::filesystem::last_write_time(folder, ec);
			for (auto &entry : std::filesystem::directory_iterator(folder, ec))
				newest = std::max(newest, entry.last_write_time(ec));
			return newest;
		}
	}

	void setLibraryRoot(const std::filesystem::path &root)
	{
		g_root = root;
		std::error_code ec;
		std::filesystem::create_directories(g_root, ec);
	}

	const CharacterLibrary &library(int charId)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_cache.find(charId);
		if (it != g_cache.end() && !it->second.folder.empty() && stampOf(it->second.folder) == g_stamp[charId])
			return it->second;

		auto lib = load(charId);
		if (!lib.folder.empty())
			g_stamp[charId] = stampOf(lib.folder);
		return g_cache[charId] = std::move(lib);
	}

	void rememberSelection(int charId, const std::wstring &customName, int slot)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_cache.find(charId);
		if (it == g_cache.end() || it->second.folder.empty())
			return;
		auto &lib = it->second;
		if (lib.lastSelected == customName && lib.lastSlot == slot)
			return;
		lib.lastSelected = customName;
		lib.lastSlot = slot;
		auto ini = (lib.folder / L"palettes.ini").wstring();
		WritePrivateProfileStringW(L"last", L"palette", customName.c_str(), ini.c_str());
		WritePrivateProfileStringW(L"last", L"slot", std::to_wstring(slot).c_str(), ini.c_str());
		g_stamp[charId] = stampOf(lib.folder);  // our own write isn't a reason to rescan
	}

	void setFallback(int charId, const std::wstring &name, int slot)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_cache.find(charId);
		if (it == g_cache.end() || it->second.folder.empty())
			return;
		auto &lib = it->second;
		for (auto &p : lib.palettes)
			if (p.name == name) {
				p.fallback = slot;
				p.fallbackIsDefault = false;
			}
		auto ini = (lib.folder / L"palettes.ini").wstring();
		WritePrivateProfileStringW(name.c_str(), L"fallback", std::to_wstring(slot).c_str(), ini.c_str());
		g_stamp[charId] = stampOf(lib.folder);
		log("library: %ls/%ls fallback set to %d", lib.folderName.c_str(), name.c_str(), slot);
	}

	void clearFallback(int charId, const std::wstring &name)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_cache.find(charId);
		if (it == g_cache.end() || it->second.folder.empty())
			return;
		auto &lib = it->second;
		for (auto &p : lib.palettes)
			if (p.name == name) {
				p.fallback = lib.defaultFallback;
				p.fallbackIsDefault = true;
			}
		auto ini = (lib.folder / L"palettes.ini").wstring();
		WritePrivateProfileStringW(name.c_str(), L"fallback", nullptr, ini.c_str());
		g_stamp[charId] = stampOf(lib.folder);
		log("library: %ls/%ls fallback cleared (default %d)", lib.folderName.c_str(), name.c_str(), lib.defaultFallback);
	}

	void setDefaultFallback(int charId, int slot)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_cache.find(charId);
		if (it == g_cache.end() || it->second.folder.empty())
			return;
		auto &lib = it->second;
		lib.defaultFallback = slot;
		for (auto &p : lib.palettes)
			if (p.fallbackIsDefault)
				p.fallback = slot;
		auto ini = (lib.folder / L"palettes.ini").wstring();
		WritePrivateProfileStringW(L"default", L"fallback", std::to_wstring(slot).c_str(), ini.c_str());
		g_stamp[charId] = stampOf(lib.folder);
		log("library: %ls default fallback set to %d", lib.folderName.c_str(), slot);
	}

	std::optional<Palette> readPaletteFile(const std::filesystem::path &file)
	{
		auto d = readAll(file);
		auto ext = lower(file.extension().wstring());

		if (ext == L".pal") {
			if (auto p = Palette::deserialize(d.data(), d.size()))
				return p;  // the game's own format (513 or 1025 bytes)
			if (d.size() == 1024) {
				// Raw 32-bit colors (0x00RRGGBB), as written by older palette editors.
				uint8_t rgb[256][3];
				for (int i = 0; i < 256; i++) {
					rgb[i][0] = d[i * 4 + 2];
					rgb[i][1] = d[i * 4 + 1];
					rgb[i][2] = d[i * 4 + 0];
				}
				return fromRgb(rgb);
			}
			return std::nullopt;
		}
		if (ext == L".act") {
			if (d.size() < 768)
				return std::nullopt;
			uint8_t rgb[256][3];
			memcpy(rgb, d.data(), 768);
			return fromRgb(rgb);
		}
		if (ext == L".bmp")
			return readBmp(d);
		return std::nullopt;
	}
}
