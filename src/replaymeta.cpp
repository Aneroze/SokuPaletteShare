#include "replaymeta.hpp"
#include "metaformat.hpp"
#include "patch.hpp"
#include "log.hpp"
#include <windows.h>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <sstream>

// Where the game saves and loads replays (th123 1.10a):
//
//   0x42B2D0  thiscall(0x898718) writeReplay(const char *path): CreateFileA(GENERIC_WRITE), writes the
//             whole file, CloseHandle, so the file is complete when it returns. Reached from the
//             game's save (0x43EBE0, also what SaveRep calls) and 0x454365. (0x42CB30 before it only
//             picks a free file name.)
//   0x42EAC0  thiscall(input manager) readReplay(const char *path): opens the file with CreateFileA.
//             Called by the replay menu (0x44B3F1) and directly by ReplayDnD, whose CreateFileA hook
//             maps that path to the real file (so our own CreateFileA of it finds the real file too).
//
// Both are hooked at their entry (patch::entry: the SEH prologue moves to a trampoline, or another
// mod's jump there is chained).

namespace ps::meta {
	namespace {
		constexpr DWORD WRITE_REPLAY = 0x42B2D0;
		constexpr DWORD READ_REPLAY = 0x42EAC0;

		std::timed_mutex g_mutex;
		std::map<std::string, nlohmann::json> g_pending;  // for the next saved replay
		std::set<std::string> g_owned;  // keys we write: removed from a sidecar when we have nothing for them
		nlohmann::json g_watched;                         // the watched replay's sidecar (object or null)

		using Method = int (__fastcall *)(void *self, void *edx, const char *path);
		Method orig_writeReplay = nullptr;
		Method orig_readReplay = nullptr;

		// "<name>.rep" -> "<name>.meta.json"
		std::wstring sidecarOf(std::wstring replay)
		{
			if (replay.size() >= 4 && !_wcsicmp(replay.c_str() + replay.size() - 4, L".rep"))
				replay.resize(replay.size() - 4);
			return replay + L".meta.json";
		}

		std::wstring wide(const char *ansi)
		{
			int n = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
			std::wstring out(n > 0 ? n - 1 : 0, L'\0');
			MultiByteToWideChar(CP_ACP, 0, ansi, -1, out.data(), n);
			return out;
		}

		// The real path of a replay the game opened by `path` (ReplayDnD passes an ANSI stand-in for
		// paths the ANSI code page can't hold, and maps it back in its CreateFileA hook).
		std::wstring realPath(const char *path)
		{
			HANDLE file = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return wide(path);
			wchar_t buffer[MAX_PATH * 2];
			DWORD n = GetFinalPathNameByHandleW(file, buffer, static_cast<DWORD>(std::size(buffer)), FILE_NAME_NORMALIZED);
			CloseHandle(file);
			return n && n < std::size(buffer) ? std::wstring(buffer, n) : wide(path);
		}

		using format::parseObject;
		constexpr size_t MAX_REPLAY = 64 * 1024 * 1024;  // far above any real replay

		// The whole file, opened like the game opened it (CreateFileA: ReplayDnD's mapping applies).
		std::optional<std::vector<uint8_t>> readWhole(const char *replay)
		{
			HANDLE file = CreateFileA(replay, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return std::nullopt;
			DWORD size = GetFileSize(file, nullptr);
			std::optional<std::vector<uint8_t>> data;
			if (size != INVALID_FILE_SIZE && size <= MAX_REPLAY) {
				data.emplace(size);
				DWORD got = 0;
				if (!ReadFile(file, data->data(), size, &got, nullptr) || got != size)
					data.reset();
			}
			CloseHandle(file);
			return data;
		}

		// Whether a replay ends exactly where the game's data ends (the game wrote all of it).
		bool endsAtGameData(const std::vector<uint8_t> &data)
		{
			auto end = format::gameDataEnd(data.data(), data.size(), data.size());
			return end && *end == data.size();
		}

		// The file's JSON object; a missing file is an empty object, an unreadable one nullopt.
		std::optional<nlohmann::json> readSidecar(const std::wstring &file)
		{
			std::ifstream in(file, std::ios::binary);
			if (!in)
				return GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES ? std::optional(nlohmann::json::object()) : std::nullopt;
			std::ostringstream text;
			text << in.rdbuf();
			if (text.str().size() > format::MAX_METADATA)
				return std::nullopt;
			return parseObject(text.str());
		}

		// Whether the game really wrote this replay: it exists and ends where its inputs end.
		bool writtenByGame(const char *replay)
		{
			auto data = readWhole(replay);
			return data && endsAtGameData(*data);
		}

		void writeMetadata(const char *replay)
		{
			std::map<std::string, nlohmann::json> pending;
			std::set<std::string> owned;
			{
				// SaveRep can save from another thread while the game is crashing: don't wait long.
				std::unique_lock<std::timed_mutex> lock(g_mutex, std::chrono::milliseconds(200));
				if (!lock) {
					log("meta: busy, %s saved without metadata", replay);
					return;
				}
				pending = g_pending;
				owned = g_owned;
			}
			if (owned.empty() || !writtenByGame(replay)) {
				if (!owned.empty())
					log("meta: %s wasn't written as expected, no metadata saved", replay);
				return;
			}
			// A sidecar may already be there: other mods' keys, or one left behind by a deleted
			// replay of the same name, whose data for our keys must not stick to this replay.
			auto file = sidecarOf(wide(replay));
			auto content = readSidecar(file);
			if (!content) {
				log("meta: %ls exists but isn't a JSON object, leaving it alone", file.c_str());
				return;
			}
			for (auto &key : owned)
				if (!pending.count(key))
					content->erase(key);
			for (auto &[key, value] : pending)
				(*content)[key] = value;
			if (content->empty()) {
				DeleteFileW(file.c_str());  // nothing left (usually: there was no file)
				return;
			}
			std::string text;
			try {
				text = content->dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
			} catch (...) {
				log("meta: can't serialize the metadata for %ls", file.c_str());
				return;
			}
			// Write next to it, then replace: a crash never leaves a half-written file.
			auto temp = file + L".tmp";
			{
				std::ofstream out(temp, std::ios::binary | std::ios::trunc);
				out << text << '\n';
				if (!out) {
					log("meta: can't write %ls", temp.c_str());
					return;
				}
			}
			bool ok = MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
			if (!ok)
				DeleteFileW(temp.c_str());
			log("meta: %ls %s (%d key(s) from us, %u bytes)", file.c_str(), ok ? "written" : "NOT written", (int)pending.size(), (unsigned)text.size());
		}

		void readWatched(const char *replay)
		{
			auto file = sidecarOf(realPath(replay));
			auto content = readSidecar(file).value_or(nlohmann::json::object());
			std::lock_guard<std::timed_mutex> lock(g_mutex);
			g_watched = content.empty() ? nlohmann::json() : content;
			if (!content.empty())
				log("meta: watching a replay with %d key(s) from %ls", (int)content.size(), file.c_str());
		}

		int __fastcall onWriteReplay(void *self, void *edx, const char *path)
		{
			int result = orig_writeReplay(self, edx, path);
			try {
				if (path)
					writeMetadata(path);
			} catch (...) {
				log("meta: saving the metadata of %s failed", path);
			}
			return result;
		}

		int __fastcall onReadReplay(void *self, void *edx, const char *path)
		{
			{
				std::lock_guard<std::timed_mutex> lock(g_mutex);
				g_watched = nlohmann::json();
			}
			int result = orig_readReplay(self, edx, path);
			try {
				if (path && (result & 0xFF))
					readWatched(path);
			} catch (...) {
				log("meta: reading the metadata of %s failed", path);
				std::lock_guard<std::timed_mutex> lock(g_mutex);
				g_watched = nlohmann::json();
			}
			return result;
		}

	}

	void setPending(const std::string &key, nlohmann::json value)
	{
		std::lock_guard<std::timed_mutex> lock(g_mutex);
		g_owned.insert(key);
		g_pending[key] = std::move(value);
	}

	void clearPending(const std::string &key)
	{
		std::lock_guard<std::timed_mutex> lock(g_mutex);
		g_owned.insert(key);
		g_pending.erase(key);
	}

	std::optional<nlohmann::json> watched(const std::string &key)
	{
		std::lock_guard<std::timed_mutex> lock(g_mutex);
		if (!g_watched.is_object())
			return std::nullopt;
		auto it = g_watched.find(key);
		if (it == g_watched.end())
			return std::nullopt;
		return *it;
	}

	void install()
	{
		orig_writeReplay = reinterpret_cast<Method>(patch::entry(WRITE_REPLAY, reinterpret_cast<const void *>(&onWriteReplay), "replay saving"));
		orig_readReplay = reinterpret_cast<Method>(patch::entry(READ_REPLAY, reinterpret_cast<const void *>(&onReadReplay), "replay loading"));
		log("meta: replay hooks %s (write %p, read %p)", orig_writeReplay && orig_readReplay ? "installed" : "INCOMPLETE",
			orig_writeReplay, orig_readReplay);
	}
}
