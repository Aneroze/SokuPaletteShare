#include "menu.hpp"
#include "game.hpp"
#include "guard.hpp"
#include "patch.hpp"
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

		int menuMove(int axis) { return game::menuMove(axis); }

		class PaletteMenu : public SokuLib::IMenu {
		public:
			explicit PaletteMenu(int charId) : _charId(charId) { refresh(); }

			void _() override {}

			int onProcess() override
			{
				int keepOpen = 0;
				if (!guarded("palette menu", [&] { keepOpen = process(); }))
					return close();
				return keepOpen;
			}

			int onRender() override
			{
				guarded("palette menu drawing", [&] { render(); });
				return true;
			}

		private:
			int process()
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
				} else if ((side || in.a == 1) && isHideRow()) {
					setHideGameSlots(_charId, !_lib.hideGameSlots);
					refresh();
					SokuLib::playSEWaveBuffer(side ? SE_MOVE : SE_OK);
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

			void render()
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
				if (!_lib.skipped.empty()) {
					// Files that look like palettes but couldn't be read, with the reason.
					constexpr size_t SHOWN = 4;
					float sy = listY + VISIBLE_ROWS * rowH + 4;
					_text.draw("Skipped (couldn't read):", listX, sy);
					for (size_t i = 0; i < std::min(SHOWN, _lib.skipped.size()); i++) {
						auto &f = _lib.skipped[i];
						_text.draw(ui::narrow(f.file) + ": " + f.reason, listX + 8, sy + 16 * (i + 1));
					}
					if (_lib.skipped.size() > SHOWN)
						_text.draw("and " + std::to_string(_lib.skipped.size() - SHOWN) + " more", listX + 8, sy + 16 * (SHOWN + 1));
				} else if (std::none_of(_lib.palettes.begin(), _lib.palettes.end(), [](auto &p) { return p.modSlot < 0; })) {
					_text.draw("No palettes yet: put .pal/.act/.bmp files in the folder.", listX, y + h - 60);
				}

				// Previews for the highlighted palette
				const float px = x + 360;
				if (isPaletteRow()) {
					auto &p = _lib.palettes[_row];
					_text.draw(p.modSlot >= 0 ? "From a palette mod (PaletteShare users see it)" : "Custom (PaletteShare users see it)", px - 10, y + 50);
					_custom.showPalette(_charId, p.palette, px + 70, y + 190, 120);
					_text.draw("Fallback " + ui::slotName(p.fallback) + " (everyone else)", px - 10, y + 210);
					_fallback.showSlot(_charId, p.fallback, px + 70, y + 350, 120);
					drawOriginalNote(p.fallback, px - 10, y + 354);
				} else if (isDefaultRow()) {
					_custom.clear();
					_text.draw("Character default fallback", px - 10, y + 210);
					_fallback.showSlot(_charId, _lib.defaultFallback, px + 70, y + 350, 120);
					drawOriginalNote(_lib.defaultFallback, px - 10, y + 354);
				} else if (isHideRow()) {
					_custom.clear();
					_fallback.clear();
					_text.draw("At character select, list:", px - 10, y + 50);
					_text.draw(_lib.hideGameSlots ? "Only your palettes" : "The game's 8 and yours", px, y + 68);
					if (_lib.hideGameSlots && _lib.palettes.empty())
						_text.draw("(the game's while you have none)", px, y + 86);
				} else {
					_custom.clear();
					_fallback.clear();
				}

				_text.draw("Up/Down select   Left/Right change   " + key(ui::Button::C) + " use default   " +
					key(ui::Button::B) + " back", x + 16, y + h - 26);
			}

		private:
			// The fallback preview shows the original colors; say so when this game's differ.
			void drawOriginalNote(int slot, float x, float y)
			{
				if (ui::slotDiffersFromOriginal(_charId, slot))
					_text.draw("Original colors (your game's differ)", x, y);
			}

			int rowCount() const { return static_cast<int>(_lib.palettes.size()) + 4; }
			bool isPaletteRow() const { return _row < static_cast<int>(_lib.palettes.size()); }
			bool isDefaultRow() const { return _row == static_cast<int>(_lib.palettes.size()); }
			bool isHideRow() const { return _row == static_cast<int>(_lib.palettes.size()) + 1; }
			bool isFolderRow() const { return _row == static_cast<int>(_lib.palettes.size()) + 2; }
			bool isCloseRow() const { return _row == static_cast<int>(_lib.palettes.size()) + 3; }

			void refresh()
			{
				_lib = library(_charId);
				addModdedSlots(_lib, false);  // "Original NN" entries have no fallback to set
			}

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
					_text.draw(ui::narrow(displayName(p)), x, y);
					_text.draw(ui::slotName(p.fallback) + (p.fallbackIsDefault ? " (default)" : ""), x + 150, y);
				} else if (r == n) {
					_text.draw("Character default", x, y);
					_text.draw(ui::slotName(_lib.defaultFallback), x + 150, y);
				} else if (r == n + 1) {
					_text.draw("Game palettes", x, y);
					_text.draw(_lib.hideGameSlots ? "Hidden" : "Shown", x + 150, y);
				} else if (r == n + 2) {
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
			// palette-picker uses the same key there; Soku2 and other added characters aren't supported.
			if (!game::palettePickerLoaded() && menuKeys().c == 1 && game::normalCharacter(charId)) {
				bool opened = guarded("menu", [&] {
					SokuLib::activateMenu(new PaletteMenu(charId));
					log("menu: palette menu for char=%d", charId);
				});
				if (opened) {
					SokuLib::playSEWaveBuffer(SE_OK);
					return true;
				}
			}
			return orig_deckMenuProcess(self);
		}
	}

	void install()
	{
		orig_deckMenuProcess = patch::pointer(DECK_MENU_PROCESS_SLOT, &onDeckMenuProcess, "palette menu");
		if (orig_deckMenuProcess)
			log("menu: installed on the deck construction menu");
	}
}
