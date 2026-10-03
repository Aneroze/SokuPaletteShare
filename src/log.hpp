#pragma once
#include <filesystem>

namespace ps {
	// Plain-text log next to the DLL (PaletteShare.log). At startup the previous one is kept as
	// PaletteShare.prev.log.
	void logOpen(const std::filesystem::path &file, bool enabled);
	void log(const char *fmt, ...);
}
