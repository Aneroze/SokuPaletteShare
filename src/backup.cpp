#include "backup.hpp"
#include "log.hpp"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace ps::backup {
	namespace fs = std::filesystem;

	namespace {
		fs::path g_library, g_backup;  // g_backup empty: no backup
		bool g_moveGone = true;         // false after a restore that failed (this run)

		std::wstring lower(std::wstring s)
		{
			std::transform(s.begin(), s.end(), s.begin(), towlower);
			return s;
		}

		// A character folder: not our _removed folder, nothing hidden.
		bool characterFolder(const fs::directory_entry &entry)
		{
			std::error_code ec;
			auto name = entry.path().filename().wstring();
			return entry.is_directory(ec) && !name.empty() && name[0] != L'_' && name[0] != L'.';
		}

		// The regular files of a folder by lowercase name (Windows names ignore case). A folder that
		// doesn't exist has none; false if it exists but couldn't be read.
		bool filesOf(const fs::path &dir, std::map<std::wstring, fs::path> &out)
		{
			std::error_code ec;
			if (!fs::exists(dir, ec))
				return !ec;
			fs::directory_iterator it(dir, ec), end;
			for (; !ec && it != end; it.increment(ec)) {
				std::error_code e;
				if (it->is_regular_file(e))
					out[lower(it->path().filename().wstring())] = it->path();
			}
			return !ec;
		}

		bool characterFolders(const fs::path &root, std::set<std::wstring> &out)
		{
			std::error_code ec;
			if (!fs::exists(root, ec))
				return !ec;
			fs::directory_iterator it(root, ec), end;
			for (; !ec && it != end; it.increment(ec))
				if (characterFolder(*it))
					out.insert(it->path().filename().wstring());
			return !ec;
		}

		bool same(const fs::path &a, const fs::path &b)
		{
			std::error_code e1, e2, e3, e4;
			auto sizeA = fs::file_size(a, e1), sizeB = fs::file_size(b, e2);
			auto timeA = fs::last_write_time(a, e3), timeB = fs::last_write_time(b, e4);
			return !e1 && !e2 && !e3 && !e4 && sizeA == sizeB && timeA == timeB;
		}

		// A read-only copy couldn't be replaced later (CopyFile copies the attribute along).
		void makeWritable(const fs::path &file)
		{
			DWORD attributes = GetFileAttributesW(file.c_str());
			if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
				SetFileAttributesW(file.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
		}

		void copy(const fs::path &from, const fs::path &to, Stats &stats)
		{
			std::error_code ec;
			fs::create_directories(to.parent_path(), ec);
			makeWritable(to);
			// Keeps the file's time (CopyFile), which is how both sides tell changed files.
			if (fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec) && !ec) {
				makeWritable(to);
				stats.copied++;
				return;
			}
			stats.failed++;
			log("backup: copying %ls to %ls failed: %s", from.c_str(), to.c_str(), ec.message().c_str());
		}

		void moveAside(const fs::path &file, const fs::path &removedDir, Stats &stats)
		{
			std::error_code ec;
			fs::create_directories(removedDir, ec);
			// Never over an earlier removed file of the same name: "Red (2).pal", "Red (3).pal"...
			auto target = removedDir / file.filename();
			for (int n = 2; fs::exists(target, ec) && n < 10000; n++)
				target = removedDir / (file.stem().wstring() + L" (" + std::to_wstring(n) + L")" + file.extension().wstring());
			fs::rename(file, target, ec);
			if (!ec) {
				stats.removed++;
				return;
			}
			stats.failed++;
			log("backup: moving %ls to %ls failed: %s", file.c_str(), removedDir.c_str(), ec.message().c_str());
		}

		void add(Stats &total, const Stats &s)
		{
			total.copied += s.copied;
			total.removed += s.removed;
			total.failed += s.failed;
		}

		void writeText(const fs::path &file, const char *text)
		{
			std::ofstream out(file, std::ios::binary | std::ios::trunc);
			out << text;
		}
	}

	Stats restore(const fs::path &from, const fs::path &to)
	{
		Stats stats;
		std::set<std::wstring> characters;
		characterFolders(from, characters);
		for (auto &name : characters) {
			std::map<std::wstring, fs::path> files, existing;
			filesOf(from / name, files);
			filesOf(to / name, existing);
			for (auto &[key, file] : files) {
				auto it = existing.find(key);
				if (it != existing.end()) {
					std::error_code e1, e2;
					auto theirs = fs::last_write_time(file, e1), ours = fs::last_write_time(it->second, e2);
					if (e1 || e2 || theirs <= ours)
						continue;  // ours is as new or newer
					copy(file, it->second, stats);
				} else {
					copy(file, to / name / file.filename(), stats);
				}
			}
		}
		return stats;
	}

	Stats mirrorCharacter(const fs::path &library, const fs::path &backup, const std::wstring &folderName, bool moveGone)
	{
		Stats stats;
		std::map<std::wstring, fs::path> files, copies;
		// Couldn't read one side: leave the backup alone (an unreadable library isn't an empty one).
		if (!filesOf(library / folderName, files) || !filesOf(backup / folderName, copies))
			return stats;
		for (auto &[key, file] : files) {
			auto it = copies.find(key);
			if (it == copies.end())
				copy(file, backup / folderName / file.filename(), stats);
			else if (!same(file, it->second))
				copy(file, it->second, stats);
		}
		if (moveGone)
			for (auto &[key, file] : copies)
				if (!files.count(key))
					moveAside(file, backup / REMOVED / folderName, stats);
		return stats;
	}

	Stats mirror(const fs::path &library, const fs::path &backup, bool moveGone)
	{
		Stats stats;
		std::set<std::wstring> characters;
		if (!characterFolders(library, characters))
			return stats;
		characterFolders(backup, characters);  // and folders gone from the library
		for (auto &name : characters)
			add(stats, mirrorCharacter(library, backup, name, moveGone));
		return stats;
	}

	void start(const fs::path &library, const fs::path &backupRoot, const fs::path &legacyLibrary)
	{
		std::error_code ec;
		fs::create_directories(library, ec);
		g_library = library;
		g_backup.clear();  // set once the backup is up to date

		// Test builds before 1.0.0: their library comes here, and it becomes the backup.
		// Only once: palettes deleted from the library later mustn't come back from it.
		auto legacyDone = legacyLibrary / L".moved";
		if (!legacyLibrary.empty() && fs::is_directory(legacyLibrary, ec) && !fs::exists(legacyDone, ec)) {
			auto moved = restore(legacyLibrary, library);
			log("backup: %d file(s) brought over from the old library %ls (%d failed)", moved.copied, legacyLibrary.c_str(), moved.failed);
			if (!moved.failed) {  // otherwise again next time
				std::error_code renamed;
				if (!backupRoot.empty() && !fs::exists(backupRoot, ec))
					fs::rename(legacyLibrary, backupRoot, renamed);
				else
					renamed = std::make_error_code(std::errc::file_exists);
				if (!renamed) {
					log("backup: the old library is now the backup");
				} else {
					writeText(legacyDone, "PaletteShare: these palettes were copied to modules\\PaletteShare\\palettes, which is the library now.\r\n");
					log("backup: the old library stays where it is, marked as brought over");
				}
			}
		}

		auto marker = library / MARKER;
		bool fresh = !fs::exists(marker, ec);
		bool restoreFailed = false;
		if (fresh && !backupRoot.empty() && fs::is_directory(backupRoot, ec)) {
			auto restored = restore(backupRoot, library);
			log("backup: new install, %d file(s) restored from %ls (%d failed)", restored.copied, backupRoot.c_str(), restored.failed);
			// Some didn't come back (e.g. open in another program): try again next start, and until
			// then nothing in the backup counts as deleted.
			restoreFailed = restored.failed > 0;
		}
		if (fresh && !restoreFailed)
			writeText(marker,
				"PaletteShare: this palette library is set up and backed up to <game folder>\\PaletteShare\\backup.\r\n"
				"Without this file, PaletteShare takes it for a new install and first copies the backup back here.\r\n");

		if (backupRoot.empty()) {
			log("backup: the game folder isn't writable, no backup of the palettes");
			return;
		}
		fs::create_directories(backupRoot, ec);
		auto stats = mirror(library, backupRoot, !restoreFailed);
		log("backup: %ls: %d file(s) copied, %d moved to %ls (%d failed)", backupRoot.c_str(), stats.copied, stats.removed, REMOVED, stats.failed);
		auto readme = backupRoot / L"README.txt";
		if (!fs::exists(readme, ec))
			writeText(readme,
				"PaletteShare's automatic backup of your palettes (modules\\PaletteShare\\palettes).\r\n"
				"\r\n"
				"Change your palettes there, not here: this folder is kept equal to it.\r\n"
				"- If the mod is removed and installed again, your palettes come back from here.\r\n"
				"- Palettes you delete there are moved to _removed here, never deleted.\r\n"
				"- To get rid of them for good (e.g. before a clean reinstall), delete this folder.\r\n");
		g_backup = backupRoot;
		g_moveGone = !restoreFailed;
	}

	void character(const std::wstring &folderName)
	{
		if (g_backup.empty() || folderName.empty())
			return;
		auto stats = mirrorCharacter(g_library, g_backup, folderName, g_moveGone);
		if (stats.copied || stats.removed || stats.failed)
			log("backup: %ls: %d file(s) copied, %d moved to %ls (%d failed)", folderName.c_str(), stats.copied, stats.removed, REMOVED, stats.failed);
	}
}
