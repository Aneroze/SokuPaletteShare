#include "menu.hpp"
#include "library.hpp"
#include "log.hpp"
#include "ui.hpp"
#include <windows.h>
#include <shellapi.h>
#include <SokuLib.hpp>
#include <algorithm>

namespace ps::menu {
	namespace {
		constexpr DWORD DECK_MENU_PROCESS_SLOT = 0x859878;  // deck construction menu vtable, onProcess
		constexpr int DECK_MENU_CHARACTER = 0xA0;           // character being edited, -1 = choosing
		constexpr int VISIBLE_ROWS = 9;
		constexpr int SE_MOVE = 0x27, SE_OK = 0x28, SE_BACK = 0x29;

		constexpr DWORD MENU_KEYS_POINTER = 0x89A394;       // KeymapManager * the game's menus read
		constexpr DWORD LOCAL_KEYS = 0x8986A8;              // player 1's menu keymap: its bindings name
		                                                    // the keys (the cluster above has none)

		int (__fastcall *orig_deckMenuProcess)(void *self) = nullptr;

		// The keys the game's own menus read (the deck construction menu reads them through this
		// pointer too); SokuLib::inputMgrs as a fallback.
		const SokuLib::KeyInput &menuKeys()
		{
			auto mgr = *reinterpret_cast<SokuLib::KeymapManager **>(MENU_KEYS_POINTER);
			return mgr ? mgr->input : SokuLib::inputMgrs.input;
		}

		std::string key(ui::Button button)
		{
			return ui::buttonName(reinterpret_cast<const SokuLib::KeymapManager *>(LOCAL_KEYS), button);
		}

		int menuMove(int axis)
		{
			if (axis == 1 || (axis > 24 && axis % 6 == 0))
				return 1;
			if (axis == -1 || (axis < -24 && -axis % 6 == 0))
				return -1;
			return 0;
		}

		class PaletteMenu : public SokuLib::IMenu {
		public:
			explicit PaletteMenu(int charId) : _charId(charId), _lib(library(charId)) {}

			void _() override {}

			int onProcess() override
			{
				auto &in = menuKeys();
				int rows = rowCount();

				if (int move = menuMove(in.verticalAxis)) {
					_row = (_row + move + rows) % rows;
					SokuLib::playSEWaveBuffer(SE_MOVE);
				}
				int side = menuMove(in.horizontalAxis);
				if (side && isPaletteRow()) {
					auto &p = _lib.palettes[_row];
					setFallback(_charId, p.name, (p.fallback + side + 8) % 8);
					refresh();
					SokuLib::playSEWaveBuffer(SE_MOVE);
				} else if (side && isDefaultRow()) {
					setDefaultFallback(_charId, (_lib.defaultFallback + side + 8) % 8);
					refresh();
					SokuLib::playSEWaveBuffer(SE_MOVE);
				}

				if (in.c == 1 && isPaletteRow() && !_lib.palettes[_row].fallbackIsDefault) {
					clearFallback(_charId, _lib.palettes[_row].name);
					refresh();
					SokuLib::playSEWaveBuffer(SE_OK);
				}
				if (in.a == 1) {
					if (isFolderRow()) {
						SokuLib::playSEWaveBuffer(SE_OK);
						ShellExecuteW(nullptr, L"explore", _lib.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
					} else if (isCloseRow()) {
						return close();
					}
				}
				if (in.b == 1)
					return close();
				return true;
			}

			int onRender() override
			{
				// Dim the deck screen, then the panel.
				ui::drawBox(0, 0, 640, 480, 0xA0000000, 0);
				const float x = 40, y = 40, w = 560, h = 400;
				ui::drawBox(x, y, w, h, 0xD0101018, 0xC0B0B0B0);
				_title.draw("PaletteShare: " + ui::narrow(_lib.folderName) + " palettes", x + 16, y + 10);

				// Rows (scrolling window)
				int rows = rowCount();
				int first = std::clamp(_row - VISIBLE_ROWS / 2, 0, std::max(0, rows - VISIBLE_ROWS));
				const float listX = x + 20, listY = y + 50, rowH = 24;
				for (int r = first; r < std::min(rows, first + VISIBLE_ROWS); r++) {
					float ry = listY + (r - first) * rowH;
					if (r == _row)
						SokuLib::MenuCursor::render(listX - 4, ry, 300);
					drawRow(r, listX, ry);
				}
				if (_lib.palettes.empty())
					_text.draw("No palettes yet: put .pal/.act/.bmp files in the folder.", listX, y + h - 60);

				// Previews for the highlighted palette
				const float px = x + 360;
				if (isPaletteRow()) {
					auto &p = _lib.palettes[_row];
					_text.draw("PaletteShare users see", px - 10, y + 50);
					_custom.showPalette(_charId, p.palette, px + 70, y + 190, 120);
					_text.draw("Others see " + ui::slotName(p.fallback), px - 10, y + 210);
					_fallback.showSlot(_charId, p.fallback, px + 70, y + 350, 120);
				} else if (isDefaultRow()) {
					_custom.clear();
					_text.draw("Default fallback", px - 10, y + 210);
					_fallback.showSlot(_charId, _lib.defaultFallback, px + 70, y + 350, 120);
				} else {
					_custom.clear();
					_fallback.clear();
				}

				_text.draw("Up/Down select   Left/Right fallback   " + key(ui::Button::C) + " use default   " +
					key(ui::Button::B) + " back", x + 16, y + h - 26);
				return true;
			}

		private:
			int rowCount() const { return static_cast<int>(_lib.palettes.size()) + 3; }
			bool isPaletteRow() const { return _row < static_cast<int>(_lib.palettes.size()); }
			bool isDefaultRow() const { return _row == static_cast<int>(_lib.palettes.size()); }
			bool isFolderRow() const { return _row == static_cast<int>(_lib.palettes.size()) + 1; }
			bool isCloseRow() const { return _row == static_cast<int>(_lib.palettes.size()) + 2; }

			void refresh() { _lib = library(_charId); }

			int close()
			{
				SokuLib::playSEWaveBuffer(SE_BACK);
				_custom.clear();
				_fallback.clear();
				return false;
			}

			void drawRow(int r, float x, float y)
			{
				int n = static_cast<int>(_lib.palettes.size());
				if (r < n) {
					auto &p = _lib.palettes[r];
					_text.draw(ui::narrow(p.name), x, y);
					_text.draw(ui::slotName(p.fallback) + (p.fallbackIsDefault ? " (default)" : ""), x + 150, y);
				} else if (r == n) {
					_text.draw("Character default", x, y);
					_text.draw(ui::slotName(_lib.defaultFallback), x + 150, y);
				} else if (r == n + 1) {
					_text.draw("Open palette folder", x, y);
				} else {
					_text.draw("Close", x, y);
				}
			}

			int _charId;
			CharacterLibrary _lib;
			int _row = 0;
			ui::Text _title{16};
			ui::Text _text{13};
			ui::PalettePreview _custom;
			ui::PalettePreview _fallback;
		};

		int __fastcall onDeckMenuProcess(void *self)
		{
			int charId = *reinterpret_cast<int *>(reinterpret_cast<char *>(self) + DECK_MENU_CHARACTER);
			if (menuKeys().c == 1 && charId >= 0) {
				SokuLib::playSEWaveBuffer(SE_OK);
				log("menu: palette menu for char=%d", charId);
				SokuLib::activateMenu(new PaletteMenu(charId));
				return true;
			}
			return orig_deckMenuProcess(self);
		}
	}

	void install()
	{
		if (GetModuleHandleA("palette-picker.dll")) {
			log("menu: palette-picker is loaded (it uses the same key there), palette menu off");
			return;
		}
		DWORD old;
		auto slot = reinterpret_cast<DWORD *>(DECK_MENU_PROCESS_SLOT);
		VirtualProtect(slot, 4, PAGE_READWRITE, &old);
		orig_deckMenuProcess = reinterpret_cast<int (__fastcall *)(void *)>(*slot);
		*slot = reinterpret_cast<DWORD>(&onDeckMenuProcess);
		VirtualProtect(slot, 4, old, &old);
		log("menu: installed on the deck construction menu");
	}
}
