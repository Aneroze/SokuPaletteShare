#pragma once
#include <windows.h>
#include <SokuLib.hpp>
#include <map>
#include <memory>
#include <string>
#include "palette.hpp"

// Small drawing helpers shared by the picker and the palette menu. All coordinates are in the
// 640x480 game frame; everything runs on the render thread.
namespace ps::ui {
	// Cached text sprites (one texture per distinct string).
	class Text {
	public:
		explicit Text(int height = 13);
		~Text();
		Text(const Text &) = delete;
		Text &operator=(const Text &) = delete;
		void draw(const std::string &text, float x, float y);

	private:
		int _height;
		std::unique_ptr<SokuLib::SWRFont> _font;
		std::map<std::string, std::unique_ptr<SokuLib::Sprite>> _sprites;
	};

	// A translucent box drawn straight on the game's device.
	void drawBox(float x, float y, float w, float h, DWORD fillArgb, DWORD lineArgb);

	// A character's select-screen sprite (stand.pat) built with a given palette.
	class PalettePreview {
	public:
		// Draws the sprite with its bottom center at (x, y), shrunk to at most maxHeight pixels.
		// A vanilla slot with the game's original colors (what players without palette mods see),
		// or as this game loads it when the archives can't be read:
		void showSlot(int charId, int slot, float x, float y, float maxHeight);
		// Any palette (a library palette):
		void showPalette(int charId, const Palette &palette, float x, float y, float maxHeight);
		void clear();

	private:
		void show(int charId, const std::string &key, const Palette *palette, int slot, float x, float y, float maxHeight);

		std::unique_ptr<SokuLib::v2::EffectManager_Select> _manager;
		SokuLib::v2::EffectObjectBase *_sprite = nullptr;
		int _charId = -1;
		std::string _key;
		float _x = -1, _y = -1;
		bool _placed = false;    // _fit/_dx/_dy measured on the first frame, then kept
		float _fit = 1, _dx = 0, _dy = 0;
	};

	std::string narrow(const std::wstring &w);
	std::string slotName(int slot);
	// Whether this game shows a slot differently from the game's original colors (a shady-loader
	// package or data file replaces it). Re-checked every 2 s; render thread.
	bool slotDiffersFromOriginal(int charId, int slot);

	// What to press for a game button, as bound in that keymap: the keyboard key's name ("Z", "A")
	// or the gamepad button ("Pad3"). Hints use this rather than the game's button letters, since
	// the D button is the A key by default.
	enum class Button { A, B, C, D };
	std::string buttonName(const SokuLib::KeymapManager *keys, Button button);
	const char *internalName(int charId);
}
