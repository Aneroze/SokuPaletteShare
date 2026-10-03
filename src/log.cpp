#include "log.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace ps {
	static FILE *g_file = nullptr;
	static std::mutex g_mutex;

	void logOpen(const std::filesystem::path &file, bool enabled)
	{
		if (!enabled || g_file)
			return;
		// Keep the previous run's log: players often restart the game before sending it.
		auto previous = file;
		previous.replace_extension(L".prev.log");
		MoveFileExW(file.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
		g_file = _wfopen(file.c_str(), L"w");
	}

	void log(const char *fmt, ...)
	{
		if (!g_file)
			return;

		SYSTEMTIME t;
		GetLocalTime(&t);
		std::lock_guard lock(g_mutex);
		fprintf(g_file, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list args;
		va_start(args, fmt);
		vfprintf(g_file, fmt, args);
		va_end(args);
		fputc('\n', g_file);
		fflush(g_file);
	}
}
