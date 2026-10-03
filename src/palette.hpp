#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace SokuLib { class Palette; }

namespace ps {
	// A character palette exactly as the game uses it: 256 colors, either 16-bit A1R5G5B5
	// (vanilla, 512 bytes) or 32-bit A8R8G8B8 (PaletteTrueColors, 1024 bytes).
	// Serialized like the game's .pal files: one bpp byte followed by the colors.
	struct Palette {
		uint8_t bpp = 16;
		std::vector<uint8_t> colors;

		static size_t colorBytes(uint8_t bpp) { return bpp == 32 ? 1024 : 512; }
		bool valid() const { return (bpp == 16 || bpp == 32) && colors.size() == colorBytes(bpp); }

		std::vector<uint8_t> serialize() const;
		static std::optional<Palette> deserialize(const uint8_t *data, size_t size);
	};

	// Content address of a palette: first 16 bytes of SHA-256 over its serialized form.
	struct PaletteHash {
		std::array<uint8_t, 16> bytes{};

		std::string hex() const;
		static std::optional<PaletteHash> fromHex(const std::string &hex);
		bool operator==(const PaletteHash &) const = default;
	};

	PaletteHash hashPalette(const Palette &palette);

	// Anti-abuse: a palette from someone else keeps its colors but takes the transparency of
	// `reference` (the palette this game would show without PaletteShare) index by index. So it
	// can't make a character invisible or translucent, or turn the transparent background into a
	// solid box.
	Palette lockTransparency(const Palette &custom, const Palette &reference);

	// `palette` with color `index` (and its transparency) taken from `from`, converted between 16 and
	// 32 bits as needed.
	Palette withColorOf(const Palette &palette, const Palette &from, int index);

	// Whether two palettes show the same colors, compared at the 16-bit palettes' 5 bits per
	// channel and ignoring transparency (so a 16-bit and a 32-bit copy of one palette match).
	bool sameColors(const Palette &a, const Palette &b);

	// Copy the palette the game currently holds / replace it (reallocating with the game's
	// allocator when the size changes). A 32-bit palette is reduced to 16-bit when no true-color
	// palette mod is loaded, since the game's sprite conversion would not understand it.
	Palette readGamePalette(const SokuLib::Palette &game);
	void writeGamePalette(const Palette &palette, SokuLib::Palette &game);
	bool trueColorSupported();

	// Content-addressed store: <dir>/<hash>.pal, plus the palettes seen this session in memory (so
	// it works without a writable folder, and a known palette costs no disk read). Loads from disk
	// verify the hash of what they read. Any thread.
	class PaletteCache {
	public:
		// Also trims the folder: leftover temp files, and the oldest palettes past MAX_FILES.
		void setDirectory(const std::filesystem::path &dir);
		// In memory and on disk.
		bool store(const Palette &palette, const PaletteHash &hash);
		// In memory only (the disk write can come later, from another thread).
		void keep(const Palette &palette, const PaletteHash &hash);
		std::optional<Palette> load(const PaletteHash &hash);

	private:
		static constexpr size_t MAX_FILES = 2000, KEEP_FILES = 1500, MAX_IN_MEMORY = 512;
		std::filesystem::path _dir;
		std::mutex _mutex;
		std::map<std::array<uint8_t, 16>, Palette> _memory;
	};
}
