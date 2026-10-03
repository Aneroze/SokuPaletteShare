#pragma once
#include <windows.h>
#include <SokuLib.hpp>

// Small things about the game (th123 1.10a) used in several places.
namespace ps::game {
	constexpr DWORD MENU_KEYS = 0x8986A8;         // player 1's keyboard menu KeymapManager
	constexpr DWORD SELECT_SCENE = 0x8A000C;      // Select * of the current character select
	constexpr DWORD CHARACTER_NAME = 0x43F3F0;    // const char *(int charId): internal name ("reimu")
	constexpr int CHARACTER_COUNT = 20;           // the normal characters, 0..19
	constexpr int CHARACTER_UTSUHO = 18;          // Okuu
	constexpr int SE_MOVE = 0x27, SE_OK = 0x28, SE_BACK = 0x29;  // playSEWaveBuffer ids

	// The character select scene object, checked by its vtable (sceneId alone can be a frame ahead
	// of the object during a scene change); null otherwise.
	SokuLib::Select *selectScene();

	// Whether a character id is one of the 20 normal characters (Soku2 and other added ones aren't
	// supported).
	inline bool normalCharacter(int charId) { return charId >= 0 && charId < CHARACTER_COUNT; }
	// The game's internal name of a character ("reimu", "udonge"), or null.
	const char *characterName(int charId);

	// The game's menu feel for a held direction: one step on press, then repeats while held.
	int menuMove(int axis);

	// palette-picker owns the palette step (and the same key in the deck screen) when it's loaded.
	// Checked on first use, once every mod is loaded (it may load after us).
	bool palettePickerLoaded();
}
