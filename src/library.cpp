#include "library.hpp"
#include "backup.hpp"
#include "game.hpp"
#include "guard.hpp"
#include "original.hpp"
#include "ui.hpp"
#include "log.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>

namespace ps {
	namespace {
		std::filesystem::path g_root;  // see setLibraryRoot
		std::mutex g_mutex;
		std::map<int, CharacterLibrary> g_cache;
		std::map<int, std::filesystem::file_time_type> g_stamp;

		std::wstring lower(std::wstring s)
		{
			std::transform(s.begin(), s.end(), s.begin(), towlower);
			return s;
		}

		// [last] is the player's own choice (player 1, or our side online); [last2p] player 2's offline.
		const wchar_t *lastSection(int who) { return who == 0 ? L"last" : L"last2p"; }

		// A palette's section: [palette:<file name>], so no file name can clash with [default] or
		// [last]. Entries without a file ("*mod 03", "*original 03") use their own name: '*' can't be
		// in a file name.
		std::wstring sectionOf(const std::wstring &name) { return name[0] == L'*' ? name : L"palette:" + name; }

		int readFallback(const std::wstring &ini, const std::wstring &name)
		{
			return static_cast<int>(GetPrivateProfileIntW(sectionOf(name).c_str(), L"fallback", -1, ini.c_str()));
		}

		// Windows writes an .ini in the ANSI code page unless the file starts with a UTF-16 byte order
		// mark, and a name the code page can't hold (e.g. Japanese on a Western system) would turn
		// into "???". So make sure it's UTF-16 (converting an ANSI one) before writing to it.
		void ensureUnicode(const std::wstring &ini)
		{
			std::ifstream in(ini, std::ios::binary);
			std::string bytes;
			if (in)
				bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			in.close();
			if (bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFF && static_cast<uint8_t>(bytes[1]) == 0xFE)
				return;
			std::wstring text;
			if (!bytes.empty()) {
				int n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
				text.resize(n);
				MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), n);
			}
			std::ofstream out(ini, std::ios::binary | std::ios::trunc);
			out.write("\xFF\xFE", 2);
			out.write(reinterpret_cast<const char *>(text.data()), text.size() * sizeof(wchar_t));
		}

		std::wstring charFolderName(int charId)
		{
			const char *name = game::characterName(charId);
			if (!name || !*name)
				return L"";
			wchar_t wide[64];
			if (!MultiByteToWideChar(CP_ACP, 0, name, -1, wide, 64))
				return L"";
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
			lib.hideGameSlots = GetPrivateProfileIntW(L"default", L"hideGamePalettes", 0, ini.c_str()) != 0;

			for (int who = 0; who < 2; who++) {
				wchar_t buffer[256];
				GetPrivateProfileStringW(lastSection(who), L"palette", L"", buffer, 256, ini.c_str());
				lib.lastSelected[who] = buffer;
				lib.lastSlot[who] = static_cast<int>(GetPrivateProfileIntW(lastSection(who), L"slot", -1, ini.c_str()));
			}

			std::vector<std::filesystem::directory_entry> entries;
			forEachEntry(lib.folder, [&](const std::filesystem::directory_entry &entry) { entries.push_back(entry); });
			std::set<std::wstring> seen;  // "Blue.pal" and "Blue.bmp": the first one
			for (auto &entry : entries) {
				if (!entry.is_regular_file(ec))
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
				if (!seen.insert(lower(entry.path().stem().wstring())).second) {
					log("library: %ls ignored, there's another palette of that name", entry.path().c_str());
					continue;
				}
				LibraryPalette item;
				item.name = entry.path().stem().wstring();
				item.file = entry.path();
				item.palette = std::move(*palette);
				item.hash = hashPalette(item.palette);
				log("library: %ls loaded (%s)", entry.path().filename().c_str(), item.hash.hex().c_str());
				int fallback = readFallback(ini, item.name);
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
			forEachEntry(folder, [&](const std::filesystem::directory_entry &entry) {
				std::error_code e;
				newest = std::max(newest, entry.last_write_time(e));
			});
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

	CharacterLibrary library(int charId)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_cache.find(charId);
		if (it != g_cache.end() && !it->second.folder.empty() && stampOf(it->second.folder) == g_stamp[charId])
			return it->second;

		auto lib = load(charId);
		if (!lib.folder.empty()) {
			g_stamp[charId] = stampOf(lib.folder);
			backup::character(lib.folderName);  // it changed (or it's the first look this run)
		}
		return g_cache[charId] = std::move(lib);
	}

	void rememberSelection(int charId, int who, const std::wstring &customName, int slot)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_cache.find(charId);
		if (it == g_cache.end() || it->second.folder.empty() || who < 0 || who > 1)
			return;
		auto &lib = it->second;
		if (lib.lastSelected[who] == customName && lib.lastSlot[who] == slot)
			return;
		lib.lastSelected[who] = customName;
		lib.lastSlot[who] = slot;
		auto ini = (lib.folder / L"palettes.ini").wstring();
		ensureUnicode(ini);
		WritePrivateProfileStringW(lastSection(who), L"palette", customName.c_str(), ini.c_str());
		WritePrivateProfileStringW(lastSection(who), L"slot", std::to_wstring(slot).c_str(), ini.c_str());
		g_stamp[charId] = stampOf(lib.folder);  // our own write isn't a reason to rescan
		backup::character(lib.folderName);
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
		ensureUnicode(ini);
		WritePrivateProfileStringW(sectionOf(name).c_str(), L"fallback", std::to_wstring(slot).c_str(), ini.c_str());
		g_stamp[charId] = stampOf(lib.folder);
		backup::character(lib.folderName);
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
		ensureUnicode(ini);
		WritePrivateProfileStringW(sectionOf(name).c_str(), nullptr, nullptr, ini.c_str());
		g_stamp[charId] = stampOf(lib.folder);
		backup::character(lib.folderName);
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
		ensureUnicode(ini);
		WritePrivateProfileStringW(L"default", L"fallback", std::to_wstring(slot).c_str(), ini.c_str());
		g_stamp[charId] = stampOf(lib.folder);
		backup::character(lib.folderName);
		log("library: %ls default fallback set to %d", lib.folderName.c_str(), slot);
	}

	void setHideGameSlots(int charId, bool hide)
	{
		std::lock_guard lock(g_mutex);
		auto it = g_cache.find(charId);
		if (it == g_cache.end() || it->second.folder.empty())
			return;
		auto &lib = it->second;
		lib.hideGameSlots = hide;
		auto ini = (lib.folder / L"palettes.ini").wstring();
		ensureUnicode(ini);
		WritePrivateProfileStringW(L"default", L"hideGamePalettes", hide ? L"1" : nullptr, ini.c_str());
		g_stamp[charId] = stampOf(lib.folder);
		backup::character(lib.folderName);
		log("library: %ls game palettes %s at character select", lib.folderName.c_str(), hide ? "hidden" : "shown");
	}

	void addModdedSlots(CharacterLibrary &lib, bool originals)
	{
		std::vector<LibraryPalette> modded, original;
		auto ini = (lib.folder / L"palettes.ini").wstring();
		auto entry = [&](std::wstring name, int slot, Palette palette) {
			LibraryPalette item;
			item.name = std::move(name);
			item.palette = std::move(palette);
			item.hash = hashPalette(item.palette);
			int fallback = lib.folder.empty() ? -1 : readFallback(ini, item.name);
			item.fallbackIsDefault = fallback < 0 || fallback > 7;
			item.fallback = item.fallbackIsDefault ? slot : fallback;
			return item;
		};
		for (int slot = 0; slot < 8; slot++) {
			auto palette = ui::moddedSlot(lib.charId, slot);
			if (!palette)
				continue;
			modded.push_back(entry(L"*mod 0" + std::to_wstring(slot), slot, std::move(*palette)));
			modded.back().modSlot = slot;
			if (!originals)
				continue;
			if (auto colors = original::palette(game::characterName(lib.charId), slot)) {
				LibraryPalette item;  // always its own slot: no fallback of its own
				item.name = L"*original 0" + std::to_wstring(slot);
				item.originalSlot = slot;
				item.palette = std::move(*colors);
				item.hash = hashPalette(item.palette);
				item.fallback = slot;
				item.fallbackIsDefault = false;
				original.push_back(std::move(item));
			}
		}
		modded.insert(modded.end(), std::make_move_iterator(original.begin()), std::make_move_iterator(original.end()));
		lib.palettes.insert(lib.palettes.begin(), std::make_move_iterator(modded.begin()), std::make_move_iterator(modded.end()));
	}

	std::wstring displayName(const LibraryPalette &palette)
	{
		if (palette.modSlot >= 0)
			return L"Mod 0" + std::to_wstring(palette.modSlot);
		if (palette.originalSlot >= 0)
			return L"Original 0" + std::to_wstring(palette.originalSlot);
		return palette.name;
	}
}
