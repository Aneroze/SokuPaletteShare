#pragma once
#include <exception>
#include <filesystem>
#include "log.hpp"

namespace ps {
	// Runs `f`, keeping any C++ exception inside: our hooks run inside the game's own code, which has
	// no handler for them (an escaping exception ends the game). Returns whether `f` finished.
	template<typename F>
	bool guarded(const char *where, F &&f) noexcept
	{
		try {
			f();
			return true;
		} catch (const std::exception &e) {
			log("%s failed: %s", where, e.what());
		} catch (...) {
			log("%s failed", where);
		}
		return false;
	}

	// Each entry of a folder, without throwing (a range-for over directory_iterator still throws
	// from its increment when the folder goes away or can't be read midway).
	template<typename F>
	void forEachEntry(const std::filesystem::path &dir, F &&f)
	{
		std::error_code ec;
		for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
			f(*it);
	}
}
