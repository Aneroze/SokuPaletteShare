#pragma once
#include <filesystem>

namespace ps {
	// Plain-text log next to the DLL (PaletteShare.log), truncated at startup.
	void logOpen(const std::filesystem::path &file, bool enabled);
	void log(const char *fmt, ...);
}
