#include "picker.hpp"
#include "hooks.hpp"
#include "library.hpp"
#include "log.hpp"
#include "net.hpp"
#include "ui.hpp"
#include <windows.h>
#include <SokuLib.hpp>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ps::picker {
	namespace {
		constexpr DWORD MENU_KEYS = 0x8986A8;       // the keyboard's menu KeymapManager
		constexpr DWORD P1_DEVICE = 0x898678;       // player 1's device: -1 keyboard, 0.. a gamepad
		constexpr DWORD GAMEPAD_KEYS = 0x899CEC;    // vector of the gamepads' KeymapManagers
		constexpr int GAMEPAD_KEYS_SIZE = 0x68;     //   (stride, see 0x43E3B0)
		constexpr DWORD SETINPUTS_CALL = 0x40A45D;  // the game's only call to KeymapManager::SetInputs
		constexpr DWORD SET_PALETTE_SELECT = 0x41FE60;  // rebuilds a player's select preview
		constexpr int KEYINPUT_OFFSET = 0x38;
		constexpr int OUTKEYS_OFFSET = 0x62;        // bits sent in netplay: up 1, down 2, left 4,
		                                            // right 8, A 16, B 32, C 64, D 128
		constexpr int STAGE_PALETTE = 1;
		constexpr int MAX_STEER_PRESSES = 16;       // then the slot is unreachable (shouldn't happen)
		constexpr int PLAYER_INFO = 0xC;            // Select: PlayerInfo[2], 0x20 bytes each:
		constexpr int INFO_SIZE = 0x20, INFO_CHARACTER = 0x8, INFO_PALETTE = 0xD;  // (see 0x4206E1)
		constexpr int STEER_TIMEOUT_FRAMES = 90;    // a press the game never applied

		enum class Steer { Idle, Pressed, Waiting };

		struct Pick {
			bool active = false;     // in the palette step and driven by us
			int charId = -1;
			CharacterLibrary lib;    // snapshot taken when the palette step started
			int index = 0;           // 0..7 vanilla slots, 8.. library palettes
			bool previewDirty = false;
			Steer steer = Steer::Idle;
			int wheelAtPress = -1;
			int waitFrames = 0;
			int presses = 0;
			int unreachable = -1;    // fallback slot the wheel couldn't reach
			// Mirror match (both sides the same character, the other one chosen): the game won't
			// let the wheel land on the other side's slot, and we skip the other side's palette.
			int takenSlot = -1;
			std::optional<Palette> takenPalette;
			bool pendingConfirm = false;
			bool editing = false;    // D: changing the highlighted palette's fallback
			const SokuLib::KeymapManager *keys = nullptr;  // drives this pick (for the key hints)
			int editSlot = 0;
			// what was confirmed, kept for the loading screen
			bool confirmed = false;
			int confirmedChar = -1;
			std::optional<Palette> confirmedCustom;

			int count() const { return 8 + static_cast<int>(lib.palettes.size()); }
			LibraryPalette *custom() { return index >= 8 ? &lib.palettes[index - 8] : nullptr; }
			const LibraryPalette *custom() const { return index >= 8 ? &lib.palettes[index - 8] : nullptr; }
			// The slot our palette asks for, and the slot it gets this match: a taken slot moves
			// to the next one, like the game bumps it (0x42084D).
			int wantedSlot() const
			{
				if (index < 8)
					return index;
				return editing ? editSlot : lib.palettes[index - 8].fallback;
			}
			int bumped(int slot) const { return slot == takenSlot ? (slot + 1) % 8 : slot; }
			int target() const { return bumped(wantedSlot()); }

			// Entries the other side of a mirror match already uses: its slot, or its palette.
			bool taken(int i) const
			{
				if (i < 8)
					return i == takenSlot;
				return takenPalette && sameColors(lib.palettes[i - 8].palette, *takenPalette);
			}
			// The nearest entry in that direction the other side doesn't use (the game's skip).
			int step(int from, int direction) const
			{
				int i = from;
				for (int n = 0; n < count(); n++) {
					i = (i + direction + count()) % count();
					if (!taken(i))
						return i;
				}
				return from;
			}
		};

		std::mutex g_mutex;  // customFor() is called from the loader thread
		Pick g_picks[2];
		bool g_disabled = false;

		void (__fastcall *orig_setInputs)(void *mgr, void *edx) = nullptr;

		SokuLib::Select *selectScene()
		{
			switch (SokuLib::sceneId) {
			case SokuLib::SCENE_SELECT: case SokuLib::SCENE_SELECTSV: case SokuLib::SCENE_SELECTCL:
				return *reinterpret_cast<SokuLib::Select **>(0x8A000C);
			default:
				return nullptr;
			}
		}

		bool online() { return SokuLib::sceneId != SokuLib::SCENE_SELECT; }

		// Menus open under the select screen (online the Vs Network menu stays on the stack), so a
		// submenu opened over it (C: profile/deck menus) is one more than that. Game thread only.
		int g_menusUnderSelect = -1;

		void trackMenus(bool inSelect)
		{
			int open = static_cast<int>(SokuLib::menuManager.size());
			if (!inSelect)
				g_menusUnderSelect = -1;
			else if (g_menusUnderSelect < 0 || open < g_menusUnderSelect)
				g_menusUnderSelect = open;
		}

		bool submenuOpen() { return g_menusUnderSelect >= 0 && static_cast<int>(SokuLib::menuManager.size()) > g_menusUnderSelect; }

		// Player 1's own device keymap (keyboard or gamepad), where the game decodes the local keys
		// before netplay sends them; looked up like 0x43EA4B does, but bounds-checked.
		SokuLib::KeymapManager *localDeviceKeys()
		{
			auto keyboard = reinterpret_cast<SokuLib::KeymapManager *>(MENU_KEYS);
			int device = *reinterpret_cast<signed char *>(P1_DEVICE);
			if (device < 0)
				return keyboard;
			auto begin = *reinterpret_cast<char **>(GAMEPAD_KEYS + 4);
			auto end = *reinterpret_cast<char **>(GAMEPAD_KEYS + 8);
			if (!begin || end < begin || device >= (end - begin) / GAMEPAD_KEYS_SIZE)
				return keyboard;
			return reinterpret_cast<SokuLib::KeymapManager *>(begin + device * GAMEPAD_KEYS_SIZE);
		}

		// The keymap that drives a side's choice, or null when we don't drive that side. Online only
		// our side, with player 1's device keymap (the scene's own keymaps are netplay streams
		// there). Offline each side its own keymap (Select::leftKeys/rightKeys, a gamepad's for
		// player 2); a side without one (Practice, VS COM) is picked by player 1, like the game does.
		SokuLib::KeymapManager *keysOf(SokuLib::Select *sel, int side)
		{
			if (online())
				return side == net::localSide() ? localDeviceKeys() : nullptr;
			auto keys = side == 0 ? sel->leftKeys : sel->rightKeys;
			return keys ? keys : sel->leftKeys;
		}

		// Whose remembered choice a side uses: the player's own, or player 2's offline.
		int memoryOf(int side) { return !online() && side == 1 ? 1 : 0; }

		char &stageOf(SokuLib::Select *sel, int player) { return player == 0 ? sel->leftSelectionStage : sel->rightSelectionStage; }
		SokuLib::InputHandler &wheelOf(SokuLib::Select *sel, int player) { return player == 0 ? sel->leftPalInput : sel->rightPalInput; }

		int characterOf(SokuLib::Select *sel, int player)
		{
			auto &input = player == 0 ? sel->leftCharInput : sel->rightCharInput;
			auto &list = sel->characterIndices;
			if (input.pos < 0 || static_cast<size_t>(input.pos) >= list.size())
				return -1;
			int id = list[input.pos];
			return id == SokuLib::CHARACTER_RANDOM || id == SokuLib::CHARACTER_NAMAZU ? -1 : id;
		}

		void startPick(Pick &pick, int side, int charId, int wheel)
		{
			pick = Pick{};
			pick.active = true;
			pick.charId = charId;
			pick.lib = library(charId);
			pick.index = wheel;
			int who = memoryOf(side);
			if (!pick.lib.lastSelected[who].empty()) {
				for (size_t i = 0; i < pick.lib.palettes.size(); i++)
					if (pick.lib.palettes[i].name == pick.lib.lastSelected[who])
						pick.index = 8 + static_cast<int>(i);
			} else if (pick.lib.lastSlot[who] >= 0 && pick.lib.lastSlot[who] < 8) {
				pick.index = pick.lib.lastSlot[who];
			}
			pick.previewDirty = true;
			log("picker: palette step for side=%d char=%d, %d custom palette(s), starting at %d", side, charId, (int)pick.lib.palettes.size(), pick.index);
		}

		// Mirror match: what the other side uses, as the game decides it (0x4204A0: only once the
		// other side chose its character, and only for the same character).
		void updateMirror(SokuLib::Select *sel, int side, Pick &pick)
		{
			pick.takenSlot = -1;
			pick.takenPalette.reset();
			int other = 1 - side;
			auto info = *reinterpret_cast<const uint8_t *const *>(reinterpret_cast<const char *>(sel) + PLAYER_INFO);
			if (!info || stageOf(sel, other) == 0)
				return;
			int mine = *reinterpret_cast<const int *>(info + side * INFO_SIZE + INFO_CHARACTER);
			int theirs = *reinterpret_cast<const int *>(info + other * INFO_SIZE + INFO_CHARACTER);
			if (mine != theirs || mine != pick.charId)
				return;
			pick.takenSlot = *reinterpret_cast<const int8_t *>(info + other * INFO_SIZE + INFO_PALETTE);

			// The other side's palette: our own pick offline, the opponent's offer online.
			auto &theirPick = g_picks[other];
			if (!online()) {
				if (theirPick.active && theirPick.charId == pick.charId) {
					if (auto custom = theirPick.custom())
						pick.takenPalette = custom->palette;
				} else if (theirPick.confirmed && theirPick.confirmedChar == pick.charId) {
					pick.takenPalette = theirPick.confirmedCustom;
				}
			} else if (auto hash = net::remotePalette(pick.charId, pick.takenSlot)) {
				pick.takenPalette = cache.load(*hash);
			}
		}

		int menuMove(int axis)
		{
			// The game's usual feel: one step on press, then repeat while held.
			if (axis == 1 || (axis > 24 && axis % 6 == 0))
				return 1;
			if (axis == -1 || (axis < -24 && -axis % 6 == 0))
				return -1;
			return 0;
		}

		// Edit mode: Up/Down choose the slot, D or A save, B cancels. Nothing reaches the game.
		void driveEdit(Pick &pick, SokuLib::KeyInput &in)
		{
			auto custom = pick.custom();
			if (int move = menuMove(in.verticalAxis))
				pick.editSlot = (pick.editSlot + move + 8) % 8;
			if (in.d == 1 || in.a == 1) {
				pick.editing = false;
				if (custom->fallback != pick.editSlot || custom->fallbackIsDefault) {
					setFallback(pick.charId, custom->name, pick.editSlot);
					custom->fallback = pick.editSlot;
					custom->fallbackIsDefault = false;
				}
				pick.presses = 0;
				pick.unreachable = -1;
				SokuLib::playSEWaveBuffer(0x28);
			} else if (in.b == 1) {
				pick.editing = false;
				pick.presses = 0;
				pick.unreachable = -1;
				SokuLib::playSEWaveBuffer(0x29);
			}
		}

		// One side's palette step, with the keys the game just decoded from that side's keymap.
		// Returns whether it drove the step.
		bool driveSide(SokuLib::Select *sel, int side, SokuLib::KeymapManager *mgr)
		{
			auto &pick = g_picks[side];
			int stage = stageOf(sel, side);
			int charId = characterOf(sel, side);
			int wheel = wheelOf(sel, side).pos;

			if (stage != STAGE_PALETTE || charId < 0) {
				if (pick.active && stage < STAGE_PALETTE)
					pick.confirmed = false;  // went back to character choice
				pick.active = false;
				return false;
			}
			bool starting = !pick.active || pick.charId != charId;
			if (starting)
				startPick(pick, side, charId, wheel);
			pick.keys = mgr;
			updateMirror(sel, side, pick);
			if (starting && pick.taken(pick.index))
				pick.index = pick.step(pick.index, 1);  // the remembered choice is the other side's

			// A submenu (C: profile/deck menus) opened over the select screen gets the keys untouched.
			if (submenuOpen()) {
				pick.pendingConfirm = false;
				pick.steer = Steer::Idle;
				pick.editing = false;
				return true;
			}

			auto &in = *reinterpret_cast<SokuLib::KeyInput *>(reinterpret_cast<char *>(mgr) + KEYINPUT_OFFSET);
			auto &bits = *reinterpret_cast<uint16_t *>(reinterpret_cast<char *>(mgr) + OUTKEYS_OFFSET);
			bool editing = pick.editing;

			if (editing) {
				driveEdit(pick, in);
			} else {
				// 1. Up/Down move our list.
				if (int move = menuMove(in.verticalAxis)) {
					pick.index = pick.step(pick.index, move);
					pick.previewDirty = true;
					pick.presses = 0;
					pick.unreachable = -1;
					pick.confirmed = false;
				}
				if (in.a == 1)
					pick.pendingConfirm = true;
				// D edits the highlighted palette's fallback (it replaces the game's random palette).
				if (in.d == 1 && pick.custom()) {
					pick.editing = true;
					pick.editSlot = pick.custom()->fallback;
					pick.pendingConfirm = false;
					SokuLib::playSEWaveBuffer(0x28);
				}
			}

			// 2. Steer the game's wheel to the target slot, one press at a time: wait until the
			// game applied a press before the next (netplay delays our own inputs too).
			int target = pick.target();
			int emitV = 0;
			switch (pick.steer) {
			case Steer::Pressed:
				pick.steer = Steer::Waiting;  // release frame
				break;
			case Steer::Waiting:
				if (wheel != pick.wheelAtPress || ++pick.waitFrames > STEER_TIMEOUT_FRAMES)
					pick.steer = Steer::Idle;
				break;
			case Steer::Idle:
				if (wheel != target && pick.unreachable != target) {
					if (pick.presses >= MAX_STEER_PRESSES) {
						pick.unreachable = target;
						log("picker: slot %d can't be reached, staying on %d", target, wheel);
						break;
					}
					int forward = (target - wheel + 8) % 8;
					emitV = forward <= 4 ? 1 : -1;
					pick.steer = Steer::Pressed;
					pick.wheelAtPress = wheel;
					pick.waitFrames = 0;
					pick.presses++;
				}
				break;
			}
			bool settled = pick.steer == Steer::Idle && (wheel == target || pick.unreachable == target);

			// 3. Confirm only once settled, so the slot we confirm is the fallback.
			int emitA = 0;
			if (!pick.editing && pick.pendingConfirm && settled) {
				emitA = 1;
				pick.pendingConfirm = false;
				pick.confirmed = true;
				pick.confirmedChar = charId;
				auto custom = pick.custom();
				pick.confirmedCustom = custom ? std::optional<Palette>(custom->palette) : std::nullopt;
				rememberSelection(charId, memoryOf(side), custom ? custom->name : L"", custom ? -1 : pick.index);
				log("picker: confirmed side=%d char=%d %ls, game slot %d", side, charId, custom ? custom->name.c_str() : L"(vanilla)", wheel);
			}

			in.verticalAxis = emitV;
			in.a = emitA;
			in.d = 0;
			bits &= ~(1 | 2 | 16 | 128);
			bits |= (emitV < 0 ? 1 : 0) | (emitV > 0 ? 2 : 0) | (emitA ? 16 : 0);
			if (editing) {
				// Keep B (and the deck/submenu keys) away from the game while editing.
				in.horizontalAxis = 0;
				in.b = 0;
				in.c = 0;
				bits &= ~(4 | 8 | 32 | 64);
			}
			return true;
		}

		// Runs once per frame for each keymap, after the game decoded its keys.
		void drive(SokuLib::KeymapManager *mgr)
		{
			auto sel = selectScene();
			trackMenus(sel != nullptr);
			std::lock_guard lock(g_mutex);
			if (!sel) {
				for (auto &p : g_picks) {
					p.active = false;
					if (SokuLib::sceneId == SokuLib::SCENE_TITLE)
						p.confirmed = false;  // a pick never outlives its session
				}
				return;
			}
			// One side per keymap and frame: in Practice player 1 picks both sides, one after the other.
			for (int side = 0; side < 2; side++)
				if (keysOf(sel, side) == mgr && driveSide(sel, side, mgr))
					break;
		}

		// SetInputs counts held frames incrementally (a key held for 2 frames reads 2), so the
		// counters we overwrite must be put back before it runs again: otherwise a held Z would
		// read as a fresh press (a == 1) on the next frame and a held arrow would step every frame.
		// Kept per keymap (keyboard, gamepads); only touched on the game thread.
		struct Override {
			SokuLib::KeyInput real;
			SokuLib::KeyInput written;
			bool active = false;
		};
		std::map<void *, Override> g_overrides;

		void __fastcall onSetInputs(void *mgr, void *edx)
		{
			auto &in = *reinterpret_cast<SokuLib::KeyInput *>(reinterpret_cast<char *>(mgr) + KEYINPUT_OFFSET);
			auto it = g_overrides.find(mgr);
			if (it != g_overrides.end() && it->second.active) {
				if (memcmp(&in, &it->second.written, sizeof(in)) == 0)
					in = it->second.real;  // untouched since our override (the game didn't reset them)
				it->second.active = false;
			}
			orig_setInputs(mgr, edx);
			if (g_disabled)
				return;
			auto &o = g_overrides[mgr];
			o.real = in;
			drive(static_cast<SokuLib::KeymapManager *>(mgr));
			o.written = in;
			o.active = memcmp(&o.real, &o.written, sizeof(in)) != 0;
		}

		// --- drawing ---------------------------------------------------------------------------

		ui::Text g_text;
		ui::PalettePreview g_customPreview[2];
		ui::PalettePreview g_fallbackPreview[2];

		std::string entryName(const Pick &pick, int index)
		{
			index = (index % pick.count() + pick.count()) % pick.count();
			return index < 8 ? ui::slotName(index) : ui::narrow(pick.lib.palettes[index - 8].name);
		}

		// Panel layout (in the 640x480 game frame): right of the game's own palette preview and deck
		// box, ending above the profile-name line. Left: header, 3 list rows, 2 info lines. Right:
		// two small previews, "You" (the custom palette) and "Others" (the fallback slot).
		// Note: the game's text renderer treats '<' as markup, so no angle brackets in strings.
		constexpr float PANEL_X = 132, PANEL_W = 300, PANEL_H = 96;
		constexpr float PANEL_Y[2] = {44, 324};
		constexpr float ROW = 14;
		constexpr float YOU_X = 196, OTHERS_X = 258;  // preview centers, relative to the panel
		constexpr float PREVIEW_HEIGHT = 75;

		// What the pick's player presses for a game button.
		std::string key(const Pick &pick, ui::Button button)
		{
			return ui::buttonName(pick.keys ? pick.keys : reinterpret_cast<const SokuLib::KeymapManager *>(MENU_KEYS), button);
		}

		void drawPick(const Pick &pick, int player)
		{
			const float x = PANEL_X, y = PANEL_Y[player];
			ui::drawBox(x, y, PANEL_W, PANEL_H, 0xB0000000, 0xC0B0B0B0);
			ui::drawBox(x + 170, y + 4, 1, PANEL_H - 8, 0x60FFFFFF, 0);

			// Header and the list: the highlighted entry with one above and one below
			g_text.draw("Palette  " + std::to_string(pick.index + 1) + "/" + std::to_string(pick.count()), x + 8, y + 2);
			if (size_t n = pick.lib.skipped.size())  // the palette menu lists them with the reason
				g_text.draw(std::to_string(n) + (n == 1 ? " file skipped" : " files skipped"), x + 92, y + 2);
			const float listY = y + 18 + ROW;
			SokuLib::MenuCursor::render(x + 6, listY, 160);
			for (int d = -1; d <= 1; d++)  // neighbors as Up/Down reach them (skipping mirror-taken ones)
				g_text.draw(entryName(pick, d ? pick.step(pick.index, d) : pick.index), x + 12, listY + ROW * d);

			// Previews
			auto custom = pick.custom();
			if (custom) {
				int slot = pick.target();
				g_text.draw("You", x + YOU_X - 10, y + 2);
				// "(original)": this game shows the slot differently (a palette mod replaces it), and
				// the preview shows what players without palette mods see instead.
				if (ui::slotDiffersFromOriginal(pick.charId, slot))
					g_text.draw("Others 0" + std::to_string(slot) + " (original)", x + OTHERS_X - 46, y + 2);
				else
					g_text.draw("Others 0" + std::to_string(slot), x + OTHERS_X - 26, y + 2);
				g_customPreview[player].showPalette(pick.charId, custom->palette, x + YOU_X, y + PANEL_H - 3, PREVIEW_HEIGHT);
				g_fallbackPreview[player].showSlot(pick.charId, slot, x + OTHERS_X, y + PANEL_H - 3, PREVIEW_HEIGHT);
			} else {
				g_customPreview[player].clear();
				g_fallbackPreview[player].clear();
				g_text.draw("Same palette", x + 186, y + 30);
				g_text.draw("for everyone", x + 186, y + 30 + ROW);
			}

			// Info lines
			const float infoY = y + PANEL_H - 2 * ROW - 4;
			if (!custom) {
				g_text.draw("Vanilla palette", x + 8, infoY);
				return;
			}
			if (pick.editing) {
				g_text.draw("Fallback: " + ui::slotName(pick.editSlot) + " ?", x + 8, infoY);
				g_text.draw("Up/Down  " + key(pick, ui::Button::D) + " save  " + key(pick, ui::Button::B) + " cancel", x + 8, infoY + ROW);
				return;
			}
			std::string fallback = "Fallback " + ui::slotName(custom->fallback) + (custom->fallbackIsDefault ? " (default)" : "");
			if (pick.bumped(custom->fallback) != custom->fallback)  // mirror match: this match only
				fallback = ui::slotName(custom->fallback) + " taken, others see 0" + std::to_string(pick.bumped(custom->fallback));
			else if (pick.unreachable == custom->fallback)
				fallback = ui::slotName(custom->fallback) + " can't be reached";
			g_text.draw(fallback, x + 8, infoY);

			switch (net::peerStatus()) {
			case net::PeerStatus::HasMod: g_text.draw("Opponent sees your palette", x + 8, infoY + ROW); break;
			case net::PeerStatus::NoMod:
			case net::PeerStatus::Incompatible: g_text.draw("Opponent sees " + ui::slotName(pick.target()), x + 8, infoY + ROW); break;
			case net::PeerStatus::Checking: g_text.draw("Opponent: checking...", x + 8, infoY + ROW); break;
			default: g_text.draw(key(pick, ui::Button::D) + ": change fallback", x + 8, infoY + ROW); break;
			}
		}

		void refreshPreview(SokuLib::Select *sel, int player, Pick &pick)
		{
			pick.previewDirty = false;
			auto rebuild = reinterpret_cast<void (__fastcall *)(SokuLib::Select *, void *, int, int, int, bool)>(SET_PALETTE_SELECT);
			rebuild(sel, nullptr, player, pick.charId, wheelOf(sel, player).pos, false);
		}

		template<int N>
		struct RenderHook {
			static inline int (__fastcall *orig)(void *self) = nullptr;

			static int __fastcall hook(void *self)
			{
				auto sel = static_cast<SokuLib::Select *>(self);
				if (!g_disabled) {
					for (int side = 0; side < 2; side++) {
						std::unique_lock lock(g_mutex);
						auto &pick = g_picks[side];
						if (pick.active && pick.previewDirty) {
							lock.unlock();  // the rebuild calls back into our preview hook
							refreshPreview(sel, side, pick);
						}
					}
				}
				int ret = orig(self);
				if (!g_disabled) {
					std::lock_guard lock(g_mutex);
					for (int side = 0; side < 2; side++) {
						if (g_picks[side].active && keysOf(sel, side) && !submenuOpen())
							drawPick(g_picks[side], side);
						else {
							g_customPreview[side].clear();
							g_fallbackPreview[side].clear();
						}
					}
				}
				return ret;
			}
		};

		template<int N>
		void hookRender(void *vtable)
		{
			auto slot = reinterpret_cast<DWORD *>(vtable) + 2;  // destructor, onProcess, onRender
			RenderHook<N>::orig = reinterpret_cast<int (__fastcall *)(void *)>(*slot);
			*slot = reinterpret_cast<DWORD>(&RenderHook<N>::hook);
		}
	}

	std::optional<Palette> customFor(int player, int charId)
	{
		if (player < 0 || player > 1)
			return std::nullopt;
		std::lock_guard lock(g_mutex);
		auto &pick = g_picks[player];
		if (pick.active && pick.charId == charId) {
			if (auto custom = pick.custom())
				return custom->palette;
			return std::nullopt;
		}
		if (pick.confirmed && pick.confirmedChar == charId)
			return pick.confirmedCustom;
		return std::nullopt;
	}

	void install()
	{
		if (GetModuleHandleA("palette-picker.dll")) {
			g_disabled = true;
			log("picker: palette-picker is loaded, so PaletteShare's picker is off");
			return;
		}

		DWORD old;
		VirtualProtect(reinterpret_cast<LPVOID>(TEXT_SECTION_OFFSET), TEXT_SECTION_SIZE, PAGE_EXECUTE_WRITECOPY, &old);
		orig_setInputs = SokuLib::TamperNearCall(SETINPUTS_CALL, onSetInputs);
		VirtualProtect(reinterpret_cast<LPVOID>(TEXT_SECTION_OFFSET), TEXT_SECTION_SIZE, old, &old);

		VirtualProtect(reinterpret_cast<LPVOID>(RDATA_SECTION_OFFSET), RDATA_SECTION_SIZE, PAGE_READWRITE, &old);
		hookRender<0>(&SokuLib::VTable_Select);
		hookRender<1>(&SokuLib::VTable_SelectServer);
		hookRender<2>(&SokuLib::VTable_SelectClient);
		VirtualProtect(reinterpret_cast<LPVOID>(RDATA_SECTION_OFFSET), RDATA_SECTION_SIZE, old, &old);
		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
		log("picker: installed (SetInputs orig=%p)", orig_setInputs);
	}
}
