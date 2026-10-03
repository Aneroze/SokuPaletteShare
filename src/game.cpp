#include "game.hpp"
#include "log.hpp"

namespace ps::game {
	SokuLib::Select *selectScene()
	{
		switch (SokuLib::sceneId) {
		case SokuLib::SCENE_SELECT: case SokuLib::SCENE_SELECTSV: case SokuLib::SCENE_SELECTCL:
			break;
		default:
			return nullptr;
		}
		auto sel = *reinterpret_cast<SokuLib::Select **>(SELECT_SCENE);
		if (!sel)
			return nullptr;
		auto vtable = *reinterpret_cast<void **>(sel);
		if (vtable != &SokuLib::VTable_Select && vtable != &SokuLib::VTable_SelectServer && vtable != &SokuLib::VTable_SelectClient)
			return nullptr;
		return sel;
	}

	const char *characterName(int charId)
	{
		if (charId < 0)
			return nullptr;
		return reinterpret_cast<const char *(*)(int)>(CHARACTER_NAME)(charId);
	}

	int menuMove(int axis)
	{
		if (axis == 1 || (axis > 24 && axis % 6 == 0))
			return 1;
		if (axis == -1 || (axis < -24 && -axis % 6 == 0))
			return -1;
		return 0;
	}

	bool palettePickerLoaded()
	{
		static int state = -1;
		if (state < 0) {
			state = GetModuleHandleA("palette-picker.dll") ? 1 : 0;
			if (state)
				log("palette-picker is loaded: PaletteShare's picker and palette menu are off (palettes are still shared)");
		}
		return state == 1;
	}
}
