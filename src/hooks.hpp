#pragma once
#include <optional>
#include "palette.hpp"

namespace ps {
	struct Config {
		// [Debug] ForcePaletteLeft / ForcePaletteRight: hash of a cached palette to force on that
		// side at character load. Exercises the same swap path received palettes will use.
		std::optional<PaletteHash> forcePalette[2];
	};

	extern Config config;
	extern PaletteCache cache;

	void installHooks();
}
