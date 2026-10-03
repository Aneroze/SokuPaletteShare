#include "replaymeta.hpp"
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
// Both start with the compiler's SEH prologue (push -1; push imm32, 7 bytes), which we move to a
// trampoline. If another mod already put a jump there, we jump to it instead.

namespace ps::meta {
	namespace {
		constexpr DWORD WRITE_REPLAY = 0x42B2D0;
		constexpr DWORD READ_REPLAY = 0x42EAC0;
		constexpr DWORD MAX_SIDECAR = 4 * 1024 * 1024;
		constexpr DWORD INPUT_COUNT_OFFSET = 0x77;  // u32, then 2 bytes per input
		constexpr DWORD INPUTS_OFFSET = 0x7B;
		constexpr char MAGIC[8] = {'S', 'O', 'K', 'U', 'M', 'E', 'T', 'A'};
		constexpr DWORD FOOTER_SIZE = 16;

		Storage g_storage = Storage::Sidecar;
		std::mutex g_mutex;
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

		// Copying or printing a JSON value recurses once per nesting level, so a file nested millions
		// of levels deep (it only takes brackets) would overflow the stack: refuse anything deeper
		// than any real metadata needs.
		constexpr int MAX_DEPTH = 32;

		bool shallowEnough(const std::string &text)
		{
			int depth = 0;
			bool inString = false, escaped = false;
			for (char c : text) {
				if (inString) {
					if (escaped)
						escaped = false;
					else if (c == '\\')
						escaped = true;
					else if (c == '"')
						inString = false;
				} else if (c == '"') {
					inString = true;
				} else if (c == '{' || c == '[') {
					if (++depth > MAX_DEPTH)
						return false;
				} else if (c == '}' || c == ']') {
					depth--;
				}
			}
			return true;
		}

		std::optional<nlohmann::json> parseObject(const std::string &text)
		{
			if (!shallowEnough(text))
				return std::nullopt;
			auto parsed = nlohmann::json::parse(text, nullptr, false);
			if (parsed.is_discarded() || !parsed.is_object())
				return std::nullopt;
			return parsed;
		}

		uint32_t crc32(const uint8_t *data, size_t size)
		{
			uint32_t crc = 0xFFFFFFFF;
			for (size_t i = 0; i < size; i++) {
				crc ^= data[i];
				for (int k = 0; k < 8; k++)
					crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
			}
			return ~crc;
		}

		uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
		void put32(std::string &out, uint32_t v) { for (int i = 0; i < 4; i++) out += static_cast<char>(v >> (8 * i) & 0xFF); }

		bool readAt(HANDLE file, DWORD offset, void *out, DWORD size)
		{
			DWORD got = 0;
			return SetFilePointer(file, offset, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
			       ReadFile(file, out, size, &got, nullptr) && got == size;
		}

		// Where the game's data ends: after the inputs. 0 when the file is too short for that.
		DWORD gameDataEnd(HANDLE file, DWORD fileSize)
		{
			uint8_t count[4];
			if (fileSize < INPUTS_OFFSET || !readAt(file, INPUT_COUNT_OFFSET, count, 4))
				return 0;
			uint64_t end = INPUTS_OFFSET + uint64_t(get32(count)) * 2;
			return end <= fileSize ? static_cast<DWORD>(end) : 0;
		}

		// The metadata appended to a .rep, if there is a valid one.
		std::optional<nlohmann::json> readRep(const char *replay)
		{
			HANDLE file = CreateFileA(replay, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return std::nullopt;
			DWORD size = GetFileSize(file, nullptr);
			DWORD end = gameDataEnd(file, size);
			uint8_t footer[FOOTER_SIZE];
			std::string text;
			bool found = end && size >= end + FOOTER_SIZE && readAt(file, size - FOOTER_SIZE, footer, FOOTER_SIZE) &&
			             !memcmp(footer + 8, MAGIC, 8) && get32(footer) <= MAX_SIDECAR && end + get32(footer) + FOOTER_SIZE == size;
			if (found) {
				text.resize(get32(footer));
				found = readAt(file, end, text.data(), static_cast<DWORD>(text.size())) &&
				        crc32(reinterpret_cast<const uint8_t *>(text.data()), text.size()) == get32(footer + 4);
			}
			CloseHandle(file);
			if (!found)
				return std::nullopt;
			return parseObject(text);
		}

		void appendToRep(const char *replay, const std::map<std::string, nlohmann::json> &pending)
		{
			nlohmann::json content = nlohmann::json::object();
			for (auto &[key, value] : pending)
				content[key] = value;
			std::string text;
			try {
				text = content.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
			} catch (...) {
				log("meta: can't serialize the metadata for %s", replay);
				return;
			}
			HANDLE file = CreateFileA(replay, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				log("meta: can't open %s to add metadata (error %lu)", replay, GetLastError());
				return;
			}
			DWORD size = GetFileSize(file, nullptr);
			DWORD end = gameDataEnd(file, size);
			if (!end || end != size) {
				log("meta: %s doesn't end where its inputs end (%lu / %lu bytes), not adding metadata", replay, end, size);
				CloseHandle(file);
				return;
			}
			std::string out = text;
			put32(out, static_cast<uint32_t>(text.size()));
			put32(out, crc32(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
			out.append(MAGIC, 8);
			DWORD written = 0;
			bool ok = SetFilePointer(file, 0, nullptr, FILE_END) != INVALID_SET_FILE_POINTER &&
			          WriteFile(file, out.data(), static_cast<DWORD>(out.size()), &written, nullptr) && written == out.size();
			CloseHandle(file);
			log("meta: %s %s (%d key(s), %u bytes appended)", replay, ok ? "got its metadata" : "FAILED to get its metadata", (int)pending.size(), (unsigned)out.size());
		}

		// The file's JSON object; a missing file is an empty object, an unreadable one nullopt.
		std::optional<nlohmann::json> readSidecar(const std::wstring &file)
		{
			std::ifstream in(file, std::ios::binary);
			if (!in)
				return GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES ? std::optional(nlohmann::json::object()) : std::nullopt;
			std::ostringstream text;
			text << in.rdbuf();
			if (text.str().size() > MAX_SIDECAR)
				return std::nullopt;
			return parseObject(text.str());
		}

		// Whether the game really wrote this replay: it exists and ends where its inputs end.
		bool writtenByGame(const char *replay)
		{
			HANDLE file = CreateFileA(replay, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			DWORD size = GetFileSize(file, nullptr);
			DWORD end = gameDataEnd(file, size);
			CloseHandle(file);
			return end && end == size;
		}

		void writeMetadata(const char *replay)
		{
			std::map<std::string, nlohmann::json> pending;
			std::set<std::string> owned;
			{
				// SaveRep can save from another thread while the game is crashing: never wait here.
				std::unique_lock lock(g_mutex, std::try_to_lock);
				if (!lock)
					return;
				pending = g_pending;
				owned = g_owned;
			}
			if (owned.empty() || !writtenByGame(replay)) {
				if (!owned.empty())
					log("meta: %s wasn't written as expected, no metadata saved", replay);
				return;
			}
			if (g_storage == Storage::Rep) {
				if (!pending.empty())
					appendToRep(replay, pending);
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
			log("meta: %ls %s (%d key(s) from us, %u bytes)", file.c_str(), ok ? "written" : "NOT written", (int)pending.size(), (unsigned)text.size());
		}

		// Both places, whatever we save to: replays come from other players too.
		void readWatched(const char *replay)
		{
			auto content = readRep(replay).value_or(nlohmann::json::object());
			size_t inRep = content.size();
			auto file = sidecarOf(realPath(replay));
			if (auto sidecar = readSidecar(file))
				for (auto &[key, value] : sidecar->items())
					content[key] = value;
			std::lock_guard lock(g_mutex);
			g_watched = content.empty() ? nlohmann::json() : content;
			if (!content.empty())
				log("meta: watching a replay with %d key(s) (%d in the .rep, %d from %ls)", (int)content.size(), (int)inRep,
					(int)(content.size() - inRep), file.c_str());
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
				std::lock_guard lock(g_mutex);
				g_watched = nlohmann::json();
			}
			int result = orig_readReplay(self, edx, path);
			try {
				if (path && (result & 0xFF))
					readWatched(path);
			} catch (...) {
				log("meta: reading the metadata of %s failed", path);
				std::lock_guard lock(g_mutex);
				g_watched = nlohmann::json();
			}
			return result;
		}

		// Jumps from a function's first 7 bytes to `hook`; returns a callable original, or null.
		void *hookEntry(DWORD at, void *hook)
		{
			auto code = reinterpret_cast<uint8_t *>(at);
			auto tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!tramp)
				return nullptr;
			bool prologue = code[0] == 0x6A && code[1] == 0xFF && code[2] == 0x68;  // push -1; push imm32
			if (prologue) {
				memcpy(tramp, code, 7);
				tramp[7] = 0xE9;
				*reinterpret_cast<DWORD *>(tramp + 8) = at + 7 - (reinterpret_cast<DWORD>(tramp) + 12);
			} else if (code[0] == 0xE9) {  // another mod's jump: chain to it
				DWORD target = at + 5 + *reinterpret_cast<int32_t *>(code + 1);
				tramp[0] = 0xE9;
				*reinterpret_cast<DWORD *>(tramp + 1) = target - (reinterpret_cast<DWORD>(tramp) + 5);
			} else {
				VirtualFree(tramp, 0, MEM_RELEASE);
				return nullptr;
			}
			DWORD old;
			VirtualProtect(code, 7, PAGE_EXECUTE_READWRITE, &old);
			code[0] = 0xE9;
			*reinterpret_cast<DWORD *>(code + 1) = reinterpret_cast<DWORD>(hook) - (at + 5);
			if (prologue)
				code[5] = code[6] = 0x90;
			VirtualProtect(code, 7, old, &old);
			FlushInstructionCache(GetCurrentProcess(), code, 7);
			return tramp;
		}
	}

	void setPending(const std::string &key, nlohmann::json value)
	{
		std::lock_guard lock(g_mutex);
		g_owned.insert(key);
		g_pending[key] = std::move(value);
	}

	void clearPending(const std::string &key)
	{
		std::lock_guard lock(g_mutex);
		g_owned.insert(key);
		g_pending.erase(key);
	}

	std::optional<nlohmann::json> watched(const std::string &key)
	{
		std::lock_guard lock(g_mutex);
		if (!g_watched.is_object())
			return std::nullopt;
		auto it = g_watched.find(key);
		if (it == g_watched.end())
			return std::nullopt;
		return *it;
	}

	void install(Storage storage)
	{
		g_storage = storage;
		orig_writeReplay = reinterpret_cast<Method>(hookEntry(WRITE_REPLAY, reinterpret_cast<void *>(&onWriteReplay)));
		orig_readReplay = reinterpret_cast<Method>(hookEntry(READ_REPLAY, reinterpret_cast<void *>(&onReadReplay)));
		log("meta: replay hooks %s (write %p, read %p), saving to %s", orig_writeReplay && orig_readReplay ? "installed" : "INCOMPLETE",
			orig_writeReplay, orig_readReplay, storage == Storage::Rep ? "the .rep" : "sidecar files");
	}
}
