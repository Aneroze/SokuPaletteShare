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

		// Palette files come from anywhere (downloads, other players), so every parser below treats
		// them as untrusted: the size is capped before reading, offsets taken from headers are
		// computed in 64 bits (a 32-bit size_t would let a crafted header wrap around a bounds check)
		// and every read is bounds-checked.
		constexpr uintmax_t MAX_FILE_SIZE = 16 * 1024 * 1024;

		struct Bytes {
			const std::vector<uint8_t> &d;

			bool has(uint64_t at, uint64_t n) const { return at <= d.size() && n <= d.size() - at; }
			uint32_t u8(uint64_t at) const { return has(at, 1) ? d[static_cast<size_t>(at)] : 0u; }
			uint32_t u16(uint64_t at) const { return u8(at) | u8(at + 1) << 8; }
			uint32_t u16be(uint64_t at) const { return u8(at) << 8 | u8(at + 1); }
			uint32_t u32(uint64_t at) const { return u16(at) | u16(at + 2) << 16; }
			bool startsWith(uint64_t at, const char *text) const
			{
				size_t n = strlen(text);
				return has(at, n) && memcmp(d.data() + at, text, n) == 0;
			}
		};

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

		uint8_t expand5(uint32_t v) { return static_cast<uint8_t>(v << 3 | v >> 2); }
		uint8_t expand6(uint32_t v) { return static_cast<uint8_t>(v << 2 | v >> 4); }

		std::optional<Palette> readBmp(const Bytes &b, const char *&why)
		{
			why = "damaged BMP";
			if (!b.has(0, 54) || !b.startsWith(0, "BM"))
				return std::nullopt;

			const uint64_t dataOffset = b.u32(10);
			const uint64_t headerSize = b.u32(14);
			const int64_t width = static_cast<int32_t>(b.u32(18));
			const int64_t height = static_cast<int32_t>(b.u32(22));
			const uint32_t bits = b.u16(28);
			const uint32_t compression = b.u32(30);
			const uint32_t used = b.u32(46);
			if (headerSize < 40 || width <= 0 || height == 0)
				return std::nullopt;
			if (compression != 0 && compression != 3) {
				why = "compressed BMP (save it uncompressed)";
				return std::nullopt;
			}

			uint8_t rgb[256][3] = {};
			if (bits == 1 || bits == 2 || bits == 4 || bits == 8) {
				// Indexed image: its color table is the palette.
				uint64_t count = used ? std::min<uint64_t>(used, 256) : uint64_t(1) << bits;
				uint64_t table = 14 + headerSize;
				if (!b.has(table, count * 4))
					return std::nullopt;
				for (uint64_t i = 0; i < count; i++) {
					rgb[i][0] = static_cast<uint8_t>(b.u8(table + i * 4 + 2));
					rgb[i][1] = static_cast<uint8_t>(b.u8(table + i * 4 + 1));
					rgb[i][2] = static_cast<uint8_t>(b.u8(table + i * 4 + 0));
				}
				return fromRgb(rgb);
			}

			// True-color swatch: the first 256 pixels, row by row from the top.
			if (bits != 16 && bits != 24 && bits != 32) {
				why = "unsupported BMP color depth";
				return std::nullopt;
			}
			bool rgb565 = false;
			if (compression == 3) {
				// BI_BITFIELDS: the masks follow the 40-byte header (V4/V5 headers keep them there too).
				uint32_t r = b.u32(54), g = b.u32(58), bl = b.u32(62);
				bool standard = bits == 16 ? (r == 0x7C00 && g == 0x03E0 && bl == 0x001F) :
				                bits == 32 ? (r == 0xFF0000 && g == 0xFF00 && bl == 0xFF) : false;
				rgb565 = bits == 16 && r == 0xF800 && g == 0x07E0 && bl == 0x001F;
				if (!standard && !rgb565) {
					why = "unsupported BMP color layout";
					return std::nullopt;
				}
			}
			const uint64_t rows = height < 0 ? -height : height;
			if (static_cast<uint64_t>(width) * rows < 256) {
				why = "BMP smaller than 256 pixels";
				return std::nullopt;
			}
			const uint64_t pixelBytes = bits / 8;
			const uint64_t stride = (static_cast<uint64_t>(width) * bits + 31) / 32 * 4;
			for (int i = 0; i < 256; i++) {
				uint64_t x = i % width, y = i / width;
				uint64_t fileRow = height > 0 ? rows - 1 - y : y;
				uint64_t at = dataOffset + fileRow * stride + x * pixelBytes;
				if (!b.has(at, pixelBytes))
					return std::nullopt;
				if (bits == 16) {
					uint32_t v = b.u16(at);
					rgb[i][0] = expand5(rgb565 ? v >> 11 & 31 : v >> 10 & 31);
					rgb[i][1] = rgb565 ? expand6(v >> 5 & 63) : expand5(v >> 5 & 31);
					rgb[i][2] = expand5(v & 31);
				} else {
					rgb[i][0] = static_cast<uint8_t>(b.u8(at + 2));
					rgb[i][1] = static_cast<uint8_t>(b.u8(at + 1));
					rgb[i][2] = static_cast<uint8_t>(b.u8(at + 0));
				}
			}
			return fromRgb(rgb);
		}

		// JASC-PAL (GIMP, Aseprite, Paint Shop Pro): text lines "JASC-PAL", "0100", the color count,
		// then one "R G B" line per color (some tools add an alpha value, ignored).
		std::optional<Palette> readJasc(const Bytes &b, const char *&why)
		{
			why = "damaged JASC palette";
			std::vector<std::string> lines;
			std::string line;
			for (uint8_t c : b.d) {
				if (c == '\n') {
					lines.push_back(std::move(line));
					line.clear();
				} else if (c != '\r') {
					line += static_cast<char>(c);
				}
				if (lines.size() > 3 + 256)
					break;
			}
			if (!line.empty())
				lines.push_back(std::move(line));
			if (lines.size() < 3)
				return std::nullopt;
			int count = atoi(lines[2].c_str());
			if (count < 1 || count > 256 || lines.size() < 3 + static_cast<size_t>(count))
				return std::nullopt;

			uint8_t rgb[256][3] = {};
			for (int i = 0; i < count; i++) {
				int r, g, bl;
				if (sscanf(lines[3 + i].c_str(), "%d %d %d", &r, &g, &bl) != 3 || r < 0 || r > 255 || g < 0 || g > 255 || bl < 0 || bl > 255)
					return std::nullopt;
				rgb[i][0] = static_cast<uint8_t>(r);
				rgb[i][1] = static_cast<uint8_t>(g);
				rgb[i][2] = static_cast<uint8_t>(bl);
			}
			return fromRgb(rgb);
		}

		// RIFF PAL (Microsoft): "RIFF" <size> "PAL ", then chunks; "data" holds a LOGPALETTE
		// (version, count, then R G B flags per color).
		std::optional<Palette> readRiff(const Bytes &b, const char *&why)
		{
			why = "damaged RIFF palette";
			uint64_t at = 12;
			for (int chunks = 0; chunks < 64 && b.has(at, 8); chunks++) {
				uint64_t size = b.u32(at + 4);
				if (b.startsWith(at, "data")) {
					uint64_t count = b.u16(at + 10);
					if (count < 1 || !b.has(at + 12, std::min<uint64_t>(count, 256) * 4))
						return std::nullopt;
					uint8_t rgb[256][3] = {};
					for (uint64_t i = 0; i < std::min<uint64_t>(count, 256); i++)
						for (int c = 0; c < 3; c++)
							rgb[i][c] = static_cast<uint8_t>(b.u8(at + 12 + i * 4 + c));
					return fromRgb(rgb);
				}
				at += 8 + size + (size & 1);  // chunks are padded to an even size
			}
			return std::nullopt;
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
				const char *why = nullptr;
				auto palette = readPaletteFile(entry.path(), &why);
				if (!palette) {
					log("library: %ls skipped: %s", entry.path().filename().c_str(), why);
					lib.skipped.push_back({entry.path().filename().wstring(), why});
					continue;
				}
				LibraryPalette item;
				item.name = entry.path().stem().wstring();
				item.file = entry.path();
				item.palette = std::move(*palette);
				item.hash = hashPalette(item.palette);
				log("library: %ls loaded (%s)", entry.path().filename().c_str(), item.hash.hex().c_str());
				int fallback = static_cast<int>(GetPrivateProfileIntW(item.name.c_str(), L"fallback", -1, ini.c_str()));
				item.fallbackIsDefault = fallback < 0 || fallback > 7;
				item.fallback = item.fallbackIsDefault ? lib.defaultFallback : fallback;
				lib.palettes.push_back(std::move(item));
			}
			std::sort(lib.palettes.begin(), lib.palettes.end(), [](auto &a, auto &b) { return lower(a.name) < lower(b.name); });
			std::sort(lib.skipped.begin(), lib.skipped.end(), [](auto &a, auto &b) { return lower(a.file) < lower(b.file); });
			log("library: %ls has %d palette(s) (%d skipped), default fallback %d", lib.folderName.c_str(), (int)lib.palettes.size(),
				(int)lib.skipped.size(), lib.defaultFallback);
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
		// Every playable character's folder up front, so players can see where palettes go. Fixed
		// names: the game's name lookup (0x43F3F0) isn't ready this early. Other characters (e.g.
		// added by other mods) still get theirs on first use.
		static const wchar_t *const characters[] = {
			L"reimu", L"marisa", L"sakuya", L"alice", L"patchouli", L"youmu", L"remilia", L"yuyuko", L"yukari", L"suika",
			L"udonge", L"aya", L"komachi", L"iku", L"tenshi", L"sanae", L"chirno", L"meirin", L"utsuho", L"suwako",
		};
		for (auto name : characters)
			std::filesystem::create_directories(g_root / name, ec);
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

	std::optional<Palette> readPaletteFile(const std::filesystem::path &file, const char **whyNot)
	{
		const char *why = "not a palette format PaletteShare knows";
		auto result = [&]() -> std::optional<Palette> {
			std::error_code ec;
			auto size = std::filesystem::file_size(file, ec);
			if (ec) {
				why = "can't open the file";
				return std::nullopt;
			}
			if (size > MAX_FILE_SIZE) {
				why = "file too large";
				return std::nullopt;
			}
			std::vector<uint8_t> d(static_cast<size_t>(size));
			std::ifstream in(file, std::ios::binary);
			if (!in.read(reinterpret_cast<char *>(d.data()), static_cast<std::streamsize>(d.size()))) {
				why = "can't open the file";
				return std::nullopt;
			}
			Bytes b{d};
			auto ext = lower(file.extension().wstring());

			if (ext == L".pal") {
				if (auto p = Palette::deserialize(d.data(), d.size()))
					return p;  // the game's own format (513 or 1025 bytes)
				if (b.startsWith(0, "JASC-PAL"))
					return readJasc(b, why);
				if (b.startsWith(0, "RIFF") && b.startsWith(8, "PAL "))
					return readRiff(b, why);
				// Anything else of at least 1024 bytes is the old Palette Editor format, like
				// palette-picker reads it: 256 colors as 0x00RRGGBB. Some versions append the
				// character's name (Shift-JIS) after them.
				if (d.size() >= 1024) {
					uint8_t rgb[256][3];
					for (int i = 0; i < 256; i++) {
						rgb[i][0] = d[i * 4 + 2];
						rgb[i][1] = d[i * 4 + 1];
						rgb[i][2] = d[i * 4 + 0];
					}
					return fromRgb(rgb);
				}
				why = "too small for any .pal format";
				return std::nullopt;
			}
			if (ext == L".act") {
				// Adobe color table: 256 RGB colors, optionally followed by a big-endian color count
				// and a transparent index (ignored: index 0 is always the transparent one).
				if (d.size() < 768) {
					why = "too small for an .act palette";
					return std::nullopt;
				}
				uint32_t count = d.size() >= 772 ? b.u16be(768) : 256;
				if (count < 1 || count > 256)
					count = 256;
				uint8_t rgb[256][3] = {};
				memcpy(rgb, d.data(), count * 3);
				return fromRgb(rgb);
			}
			if (ext == L".bmp")
				return readBmp(b, why);
			return std::nullopt;
		}();
		if (!result && whyNot)
			*whyNot = why;
		return result;
	}
}
