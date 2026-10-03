#pragma once
#include <filesystem>
#include <optional>
#include "palette.hpp"

// Reading the palette files players put in their library (see library.hpp for the formats). No game
// access, so it can be tested on its own.
namespace ps {
	// whyNot (optional) gets a short reason when the file can't be read.
	std::optional<Palette> readPaletteFile(const std::filesystem::path &file, const char **whyNot = nullptr);
}
