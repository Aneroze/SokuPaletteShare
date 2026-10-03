#include "log.hpp"
#include <windows.h>
#include <cstdarg>
#include <clocale>
#include <cstdio>
#include <mutex>
#include <string>

namespace ps {
	static FILE *g_file = nullptr;
	static std::mutex g_mutex;
	// %ls (file and palette names) is converted with the format's locale: the default "C" locale
	// stops at the first non-ASCII character, so format with a UTF-8 one (the log is UTF-8). A
	// locale object of our own, so the game's global locale stays untouched.
	static _locale_t g_utf8 = nullptr;
	static long g_written = 0;
	static constexpr long MAX_LOG = 2 * 1024 * 1024;  // then it stops (a whole session fits easily)

	void logOpen(const std::filesystem::path &file, bool enabled)
	{
		if (!enabled || g_file)
			return;
		// Keep the previous run's log: players often restart the game before sending it.
		auto previous = file;
		previous.replace_extension(L".prev.log");
		MoveFileExW(file.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
		g_file = _wfopen(file.c_str(), L"w");
		g_utf8 = _create_locale(LC_ALL, ".UTF-8");
	}

	void log(const char *fmt, ...)
	{
		if (!g_file)
			return;

		SYSTEMTIME t;
		GetLocalTime(&t);
		std::lock_guard lock(g_mutex);
		if (g_written > MAX_LOG)
			return;
		long before = ftell(g_file);
		fprintf(g_file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list args;
		va_start(args, fmt);
		if (g_utf8)
			_vfprintf_l(g_file, fmt, g_utf8, args);
		else
			vfprintf(g_file, fmt, args);
		va_end(args);
		fputc('\n', g_file);
		g_written += ftell(g_file) - before;
		if (g_written > MAX_LOG)
			fputs("(log full: nothing more is written until the game restarts)\n", g_file);
		fflush(g_file);
	}

	void logEnvironmentOnce()
	{
		static std::once_flag once;
		std::call_once(once, [] {
			// Windows' real version (GetVersionEx lies to programs without a manifest), and Wine's.
			RTL_OSVERSIONINFOW os{sizeof(os)};
			HMODULE ntdll = GetModuleHandleA("ntdll.dll");
			if (auto get = reinterpret_cast<LONG (WINAPI *)(RTL_OSVERSIONINFOW *)>(GetProcAddress(ntdll, "RtlGetVersion")))
				get(&os);
			auto wine = reinterpret_cast<const char *(*)()>(GetProcAddress(ntdll, "wine_get_version"));
			std::string mods;
			for (const char *name : {"giuroll.dll", "palette-picker.dll", "PaletteTrueColors.dll", "true-color-palettes.dll", "shady-loader.dll",
			                         "SokuLobbiesMod.dll", "InGameHostlist.dll", "Autopunch.dll", "ReplayDnD.dll", "SaveRep.dll",
			                         "InfiniteDecks.dll", "Soku2.dll", "SokuEngine.dll", "CharacterEngine.dll"})
				if (GetModuleHandleA(name))
					mods += std::string(mods.empty() ? "" : ", ") + name;
			log("environment: Windows %lu.%lu.%lu%s%s; mods: %s", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber,
				wine ? ", Wine " : "", wine ? wine() : "", mods.empty() ? "(none of note)" : mods.c_str());
		});
	}
}
