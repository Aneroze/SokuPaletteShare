#include "picker.hpp"
#include "game.hpp"
#include "guard.hpp"
#include "patch.hpp"
#include "hooks.hpp"
#include "library.hpp"
#include "log.hpp"
#include "net.hpp"
#include "selectguard.hpp"
#include "ui.hpp"
#include <windows.h>
#include <SokuLib.hpp>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ps::picker {
	namespace {
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
		// Our presses are held like a key until the game applies them: online a one-frame press can
		// get lost on the way (packet loss), a held one can't. At most MAX_HOLD_FRAMES (the game
		// repeats a direction held for 30), then released for at least MIN_RELEASE_FRAMES online so
		// the next one reads as a new press.
		constexpr int MAX_HOLD_FRAMES = 20;
		constexpr int MIN_RELEASE_FRAMES = 3;
		constexpr int STEER_TIMEOUT_FRAMES = 60;    // a palette step the game never applied: again
		constexpr int CONFIRM_TIMEOUT_FRAMES = 90;  // a confirm the game never applied: again,
		constexpr int CONFIRM_RETRIES = 2;          //   at most this many times
		constexpr DWORD WHEEL_SOUND_CALL = 0x4206BE;  // palette step: playSEWaveBuffer(0x27) when the
		                                              // deck or palette cursor moved

		enum class Steer { Idle, Pressed, Waiting };

		struct Pick {
			bool active = false;     // in the palette step and driven by us
			int charId = -1;
			CharacterLibrary lib;    // snapshot taken when the palette step started
			int index = 0;           // 0..7 vanilla slots, 8.. library palettes
			// A slot a palette mod replaces in this game works like a library palette with its own
			// fallback (its own slot unless the player sets another); null for a normal slot.
			std::optional<LibraryPalette> mods[8];
			// This game's palette for each slot, to preview a slot before the wheel reaches it.
			std::optional<Palette> slotPalettes[8];
			// The player hides the game's palettes for this character (palette menu) and has some
			// of their own: the list skips the game's slots and "Original NN" entries.
			bool hideGameSlots = false;
			bool previewDirty = false;
			Steer steer = Steer::Idle;
			int wheelAtPress = -1;
			int waitFrames = 0;
			int pressDir = 0;        // the press being held: +1 down, -1 up
			int releaseFrames = 0;
			int presses = 0;
			int unreachable = -1;    // fallback slot the wheel couldn't reach
			// Mirror match (both sides the same character, the other one chosen): the game won't
			// let the wheel land on the other side's slot, and we skip the other side's palette.
			int takenSlot = -1;
			std::optional<Palette> takenPalette;
			bool pendingConfirm = false;
			// The confirm is D (the game's random deck, which also confirms) instead of A.
			bool confirmWithD = false;
			// Frames since we sent the confirm (A or D), -1 = none sent. Online the game applies it only
			// after the netplay delay; until then the list and the wheel must not move, or the slot
			// (and our confirmed palette) would change under the confirm.
			int confirmSent = -1;
			int confirmRetries = 0;
			bool inSubmenu = false;  // a submenu was open over the select screen last frame
			bool editing = false;    // changing the highlighted palette's fallback
			const SokuLib::KeymapManager *keys = nullptr;  // drives this pick (for the key hints)
			int editSlot = 0;
			// what was confirmed, kept for the loading screen
			bool confirmed = false;
			int confirmedChar = -1;
			std::optional<Palette> confirmedCustom;
			// The game confirmed without us: rebuild the preview with what it confirmed.
			bool previewAfterConfirm = false;

			int count() const { return 8 + static_cast<int>(lib.palettes.size()); }
			// The entry's palette with a fallback of its own (library or replaced slot), or null.
			const LibraryPalette *entry(int i) const
			{
				if (i < 8)
					return mods[i] ? &*mods[i] : nullptr;
				return &lib.palettes[i - 8];
			}
			LibraryPalette *custom() { return const_cast<LibraryPalette *>(entry(index)); }
			const LibraryPalette *custom() const { return entry(index); }
			// The slot our palette asks for, and the slot it gets this match: a taken slot moves
			// to the next one, like the game bumps it (0x42084D).
			int wantedSlot() const
			{
				auto c = custom();
				if (!c)
					return index;
				return editing ? editSlot : c->fallback;
			}
			int bumped(int slot) const { return slot == takenSlot ? (slot + 1) % 8 : slot; }
			int target() const { return bumped(wantedSlot()); }

			// Whether the list shows the entry.
			bool listed(int i) const
			{
				if (!hideGameSlots)
					return true;
				auto p = entry(i);
				return p && p->originalSlot < 0;
			}
			int listedCount() const
			{
				int n = 0;
				for (int i = 0; i < count(); i++)
					n += listed(i);
				return n;
			}
			int listedPosition() const  // 1-based, of the highlighted entry
			{
				int n = 0;
				for (int i = 0; i <= index && i < count(); i++)
					n += listed(i);
				return n;
			}
			// Entries the other side of a mirror match already uses: its slot, or its palette.
			bool taken(int i) const
			{
				if (i < 8 && i == takenSlot)
					return true;
				auto p = entry(i);
				return p && takenPalette && sameColors(p->palette, *takenPalette);
			}
			// The nearest listed entry in that direction the other side doesn't use (the game's skip).
			int step(int from, int direction) const
			{
				int i = from;
				for (int n = 0; n < count(); n++) {
					i = (i + direction + count()) % count();
					if (listed(i) && !taken(i))
						return i;
				}
				return from;
			}
		};

		std::mutex g_mutex;  // customFor() is called from the loader thread
		Pick g_picks[2];
		// Online: our presses that change the select stage and haven't been applied yet (selectguard.hpp).
		SelectGuard g_guards[2];
		bool disabled() { return game::palettePickerLoaded(); }

		void (__fastcall *orig_setInputs)(void *mgr, void *edx) = nullptr;

		// The palette step's move sound comes from us for the sides we drive: one per move of our
		// list, right away, instead of the game's one per wheel step (none for a list move the wheel
		// doesn't need, several later for one that needs several). Off if the hook isn't in.
		DWORD g_origWheelSound = 0;
		void moveSound()
		{
			if (g_origWheelSound)
				SokuLib::playSEWaveBuffer(game::SE_MOVE);
		}

		SokuLib::Select *selectScene() { return game::selectScene(); }

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
			auto keyboard = reinterpret_cast<SokuLib::KeymapManager *>(game::MENU_KEYS);
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
			return game::normalCharacter(id) ? id : -1;
		}

		// The pick's snapshot of the character's palettes: library files, replaced slots, slot colors.
		int loadLibrary(Pick &pick, int charId)
		{
			pick.lib = library(charId);
			for (auto &m : pick.mods)
				m.reset();
			addModdedSlots(pick.lib, config.showOriginalPalettes);
			int modded = 0;
			for (auto it = pick.lib.palettes.begin(); it != pick.lib.palettes.end();) {
				if (it->modSlot >= 0) {
					pick.mods[it->modSlot] = std::move(*it);
					it = pick.lib.palettes.erase(it);
					modded++;
				} else {
					++it;
				}
			}
			for (int slot = 0; slot < 8; slot++)
				pick.slotPalettes[slot] = ui::gameSlot(charId, slot);
			pick.hideGameSlots = pick.lib.hideGameSlots;
			if (pick.hideGameSlots && pick.listedCount() == 0)
				pick.hideGameSlots = false;  // none of their own: the game's palettes after all
			return modded;
		}

		void startPick(Pick &pick, int side, int charId, int wheel)
		{
			pick = Pick{};
			pick.active = true;
			pick.charId = charId;
			int modded = loadLibrary(pick, charId);
			pick.index = wheel;
			int who = memoryOf(side);
			if (!pick.lib.lastSelected[who].empty()) {
				for (int i = 0; i < pick.count(); i++)
					if (auto p = pick.entry(i); p && p->name == pick.lib.lastSelected[who])
						pick.index = i;
			} else if (pick.lib.lastSlot[who] >= 0 && pick.lib.lastSlot[who] < 8) {
				pick.index = pick.lib.lastSlot[who];
			}
			pick.previewDirty = true;
			log("picker: palette step for side=%d char=%d, %d custom palette(s), %d slot(s) replaced by palette mods, starting at %d%s", side,
				charId, (int)pick.lib.palettes.size(), modded, pick.index, pick.hideGameSlots ? ", game palettes hidden" : "");
		}

		// After a submenu (e.g. the palette menu from the deck screen): fallbacks or files may have
		// changed. Reload, staying on the same entry (by slot or file name) where it still exists.
		void reloadPick(Pick &pick)
		{
			std::wstring name;
			if (pick.index >= 8)
				name = pick.lib.palettes[pick.index - 8].name;
			int slot = pick.index < 8 ? pick.index : -1;
			loadLibrary(pick, pick.charId);
			pick.index = slot >= 0 ? slot : 0;
			for (int i = 8; i < pick.count(); i++)
				if (pick.lib.palettes[i - 8].name == name)
					pick.index = i;
			if (!pick.listed(pick.index) || pick.taken(pick.index))  // e.g. the game's palettes were just hidden
				pick.index = pick.step(pick.index, 1);
			pick.previewDirty = true;
			pick.presses = 0;
			pick.unreachable = -1;
			log("picker: palettes reloaded after a submenu, now at %d", pick.index);
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
			} else {
				pick.takenPalette = net::received(other, pick.charId, pick.takenSlot);
			}
		}

		int menuMove(int axis) { return game::menuMove(axis); }

		// The fallback button's held frames (0 when it's off).
		int fallbackPressed(SokuLib::KeyInput &in)
		{
			const int *button = ui::buttonInput(in, config.fallbackButton);
			return button ? *button : 0;
		}

		// Keeps the fallback button away from the game.
		void swallowFallbackButton(SokuLib::KeyInput &in, uint16_t &bits)
		{
			if (int *button = ui::buttonInput(in, config.fallbackButton))
				*button = 0;
			bits &= ~ui::buttonBit(config.fallbackButton);
		}

		// Edit mode: Up/Down choose the slot, the fallback button or A save, B cancels. Nothing
		// reaches the game.
		void driveEdit(Pick &pick, SokuLib::KeyInput &in)
		{
			auto custom = pick.custom();
			if (int move = menuMove(in.verticalAxis)) {
				pick.editSlot = (pick.editSlot + move + 8) % 8;
				moveSound();
			}
			if (fallbackPressed(in) == 1 || in.a == 1) {
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

		// The palette step ended with a confirm that wasn't ours (the guard should prevent it; this
		// keeps what we load and show true to what the game picked, whatever got through).
		void confirmedByGame(Pick &pick, int side, int wheel)
		{
			if (pick.confirmed && pick.confirmSent >= 0)
				return;  // ours
			auto custom = pick.custom();
			bool keep = custom && wheel == pick.target();  // the wheel was where the picker wanted it
			pick.confirmed = true;
			pick.confirmedChar = pick.charId;
			pick.confirmedCustom = keep ? std::optional<Palette>(custom->palette) : std::nullopt;
			pick.previewAfterConfirm = true;
			log("picker: the game confirmed side=%d char=%d without the picker: game slot %d, highlighted %d (%ls), %s", side, pick.charId,
				wheel, pick.index, custom ? displayName(*custom).c_str() : L"vanilla", keep ? "custom palette kept" : "loading the game slot");
		}

		void logGuard(SelectGuard::Event e, int side, const SelectGuard &guard)
		{
			switch (e) {
			case SelectGuard::Event::Queued:
				log("picker: side=%d pressed during the netplay delay, held back for the palette step (confirm %d%s, moves %+d)", side,
					(int)guard.queued().confirm, guard.queued().confirmWithD ? " D" : "", guard.queued().moves);
				break;
			case SelectGuard::Event::Sent: log("picker: side=%d character confirm on its way, holding confirms back until the palette step", side); break;
			case SelectGuard::Event::Cancelled: log("picker: side=%d went back during the netplay delay, nothing held back", side); break;
			case SelectGuard::Event::TimedOut: log("picker: side=%d a select press was never applied, guard released", side); break;
			default: break;
			}
		}

		// One side's palette step, with the keys the game just decoded from that side's keymap.
		// Returns whether it drove the step.
		bool driveSide(SokuLib::Select *sel, int side, SokuLib::KeymapManager *mgr)
		{
			auto &pick = g_picks[side];
			auto &guard = g_guards[side];
			int stage = stageOf(sel, side);
			int charId = characterOf(sel, side);
			int wheel = wheelOf(sel, side).pos;
			auto &in = *reinterpret_cast<SokuLib::KeyInput *>(reinterpret_cast<char *>(mgr) + KEYINPUT_OFFSET);
			auto &bits = *reinterpret_cast<uint16_t *>(reinterpret_cast<char *>(mgr) + OUTKEYS_OFFSET);
			GuardKeys keys{in.verticalAxis, in.a, in.b, in.d, bits};
			const bool dConfirms = config.fallbackButton != ui::Button::D;
			// Online our keys act only after the netplay delay (selectguard.hpp); offline right away.
			const bool delayed = online();
			if (delayed)
				logGuard(guard.observe(stage), side, guard);

			if (stage != STAGE_PALETTE || charId < 0) {
				if (pick.active && stage > STAGE_PALETTE)
					confirmedByGame(pick, side, wheel);
				if (pick.active && stage < STAGE_PALETTE)
					pick.confirmed = false;  // went back to character choice
				pick.active = false;
				if (delayed && !submenuOpen()) {
					if (stage == STAGE_PALETTE && !guard.inFlight()) {
						// A character the picker doesn't handle: a confirm held back goes to the game as it is.
						auto q = guard.take();
						guard.suppressHeld(keys, true);
						if (q.confirm) {
							in.a = !q.confirmWithD;
							in.d = q.confirmWithD;
							bits |= q.confirmWithD ? SelectGuard::BIT_D : SelectGuard::BIT_A;
						}
					} else {
						logGuard(guard.filter(keys, stage, menuMove(in.verticalAxis), dConfirms, charId >= 0), side, guard);
					}
				}
				return false;
			}
			bool starting = !pick.active || pick.charId != charId;
			if (starting)
				startPick(pick, side, charId, wheel);
			pick.keys = mgr;
			updateMirror(sel, side, pick);
			if (starting && (pick.taken(pick.index) || !pick.listed(pick.index)))
				pick.index = pick.step(pick.index, 1);  // the remembered choice is the other side's, or hidden

			// A submenu (C: profile/deck menus) opened over the select screen gets the keys untouched.
			if (submenuOpen()) {
				pick.pendingConfirm = false;
				pick.steer = Steer::Idle;
				pick.editing = false;
				pick.inSubmenu = true;
				return true;
			}
			if (pick.inSubmenu) {
				pick.inSubmenu = false;
				reloadPick(pick);
			}

			if (delayed) {
				if (guard.inFlight()) {
					// Presses of ours are still on their way (B back to the character step, maybe an A
					// again): the picker sends nothing, the keys go to the game as they are.
					pick.steer = Steer::Idle;
					pick.pendingConfirm = false;
					logGuard(guard.filter(keys, stage, menuMove(in.verticalAxis), dConfirms, true), side, guard);
					return true;
				}
				if (auto q = guard.take(); q.any()) {
					// Pressed before the palette step arrived: the picker takes them from here.
					for (int n = 0; n < (q.moves < 0 ? -q.moves : q.moves); n++)
						pick.index = pick.step(pick.index, q.moves < 0 ? -1 : 1);
					if (q.moves) {
						pick.previewDirty = true;
						moveSound();
					}
					if (q.confirm) {
						pick.pendingConfirm = true;
						pick.confirmWithD = q.confirmWithD;
						pick.confirmRetries = 0;
					}
					log("picker: side=%d palette step arrived with %+d move(s)%s held back, now at %d", side, q.moves,
						q.confirm ? (q.confirmWithD ? " and a confirm (D)" : " and a confirm") : "", pick.index);
				}
				guard.suppressHeld(keys, false);
			}

			// Confirm sent, the game hasn't applied it yet: keep everything still (B, the deck keys
			// and C still reach the game).
			if (pick.confirmSent >= 0) {
				if (++pick.confirmSent <= CONFIRM_TIMEOUT_FRAMES) {
					in.verticalAxis = 0;
					swallowFallbackButton(in, bits);
					bits &= ~(1 | 2);
					// The confirm stays held like a key until the game takes it (we stop here once it did).
					bool hold = pick.confirmSent < MAX_HOLD_FRAMES;
					in.a = hold && !pick.confirmWithD ? pick.confirmSent + 1 : 0;
					in.d = hold && pick.confirmWithD ? pick.confirmSent + 1 : 0;
					bits = (bits & ~(16 | 128)) | (hold ? (pick.confirmWithD ? 128 : 16) : 0);
					return true;
				}
				pick.confirmSent = -1;
				if (pick.confirmRetries < CONFIRM_RETRIES) {
					pick.confirmRetries++;
					pick.pendingConfirm = true;
					log("picker: the confirm wasn't applied, sending it again");
				} else {
					log("picker: the confirm wasn't applied, picking again");
				}
			}
			bool editing = pick.editing;

			if (editing) {
				driveEdit(pick, in);
			} else {
				// 1. Up/Down move our list.
				if (int move = menuMove(in.verticalAxis)) {
					pick.index = pick.step(pick.index, move);
					pick.previewDirty = true;
					moveSound();
					pick.presses = 0;
					pick.unreachable = -1;
					pick.confirmed = false;
				}
				// Online, B goes back to the character step only after the netplay delay: until then the
				// picker sends nothing, or its presses would act on the character step.
				if (delayed && in.b == 1) {
					guard.backSent();
					pick.steer = Steer::Idle;
					pick.pendingConfirm = false;
					return true;
				}
				// A confirms; so does D, with the game's random deck. Either is sent on once the wheel
				// is on the slot to confirm.
				if (in.a == 1 || (dConfirms && in.d == 1)) {
					pick.pendingConfirm = true;
					pick.confirmWithD = in.a != 1;
					pick.confirmRetries = 0;
				}
				// The fallback button edits the highlighted palette's fallback.
				if (fallbackPressed(in) == 1 && pick.custom() && pick.custom()->originalSlot < 0) {
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
				if (wheel == pick.wheelAtPress && ++pick.waitFrames < MAX_HOLD_FRAMES) {
					emitV = pick.pressDir * (pick.waitFrames + 1);  // counts up like a held key
					break;
				}
				pick.steer = Steer::Waiting;  // applied (or held long enough): release
				pick.releaseFrames = 0;
				break;
			case Steer::Waiting:
				pick.releaseFrames++;
				if (wheel != pick.wheelAtPress) {
#ifdef PS_STEER_LOG
					if (pick.releaseFrames == 1)
						log("steer: applied after %d frames, wheel %d (target %d)", pick.waitFrames, wheel, target);
#endif
					if (pick.releaseFrames >= (online() ? MIN_RELEASE_FRAMES : 1))
						pick.steer = Steer::Idle;
				} else if (++pick.waitFrames > STEER_TIMEOUT_FRAMES) {
					log("picker: a palette step wasn't applied (wheel still on %d), trying again", wheel);
					pick.steer = Steer::Idle;
				}
				break;
			case Steer::Idle:
				if (wheel != target && pick.unreachable != target) {
					if (pick.presses >= MAX_STEER_PRESSES) {
						pick.unreachable = target;
						log("picker: slot %d can't be reached, staying on %d", target, wheel);
						break;
					}
					int forward = (target - wheel + 8) % 8;
					pick.pressDir = forward <= 4 ? 1 : -1;
					emitV = pick.pressDir;
#ifdef PS_STEER_LOG
					log("steer: press %+d wheel %d target %d (real axis %d)", emitV, wheel, target, in.verticalAxis);
#endif
					pick.steer = Steer::Pressed;
					pick.wheelAtPress = wheel;
					pick.waitFrames = 0;
					pick.presses++;
				}
				break;
			}
			bool settled = pick.steer == Steer::Idle && (wheel == target || pick.unreachable == target);

			// 3. Confirm only once settled, so the slot we confirm is the fallback.
			int emitConfirm = 0;
			if (!pick.editing && pick.pendingConfirm && settled) {
				emitConfirm = 1;
				pick.pendingConfirm = false;
				pick.confirmSent = 0;
				pick.confirmed = true;
				pick.confirmedChar = charId;
				auto custom = pick.custom();
				pick.confirmedCustom = custom ? std::optional<Palette>(custom->palette) : std::nullopt;
				rememberSelection(charId, memoryOf(side), custom ? custom->name : L"", custom ? -1 : pick.index);
				log("picker: confirmed side=%d char=%d %ls, game slot %d%s", side, charId, custom ? displayName(*custom).c_str() : L"(vanilla)", wheel,
					pick.confirmWithD ? " (D: random deck)" : "");
			}

			swallowFallbackButton(in, bits);
			in.verticalAxis = emitV;
			in.a = emitConfirm && !pick.confirmWithD;
			in.d = emitConfirm && pick.confirmWithD;
			bits &= ~(1 | 2 | 16 | 128);
			bits |= (emitV < 0 ? 1 : 0) | (emitV > 0 ? 2 : 0) | (in.a ? 16 : 0) | (in.d ? 128 : 0);
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
					p.previewAfterConfirm = false;
					if (SokuLib::sceneId == SokuLib::SCENE_TITLE)
						p.confirmed = false;  // a pick never outlives its session
				}
				for (auto &g : g_guards)
					g = SelectGuard{};
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
			if (disabled())
				return;
			auto &o = g_overrides[mgr];
			o.real = in;
			if (!guarded("picker", [&] { drive(static_cast<SokuLib::KeymapManager *>(mgr)); }))
				in = o.real;  // whatever happened, the game gets the player's own keys
			o.written = in;
			o.active = memcmp(&o.real, &o.written, sizeof(in)) != 0;
		}

		// The game's palette step played its move sound: moved = bit 0 deck cursor, bit 1 palette
		// wheel. Whether to let it play.
		bool __cdecl onWheelSound(int moved, int side)
		{
			if (disabled() || (moved & 3) != 2 || side < 0 || side > 1)
				return true;  // a deck move, or not ours
			std::lock_guard lock(g_mutex);
#ifdef PS_STEER_LOG
			log("steer: wheel sound side=%d %s", side, g_picks[side].active ? "muted" : "played");
#endif
			return !g_picks[side].active;  // our steering: the list move already played it
		}

		__declspec(naked) void wheelSoundStub()
		{
			__asm {
				pushad    // whatever runs next (the game, or another mod's hook here) sees the registers as they were
				push ebp  // the side the select loop is on
				push eax  // what moved (0x41FC70's result)
				call onWheelSound
				add esp, 8
				test al, al
				popad     // (leaves the flags alone)
				jz skip
				jmp g_origWheelSound  // playSEWaveBuffer(0x27), still on the stack
			skip:
				ret
			}
		}

		// --- drawing ---------------------------------------------------------------------------

		// Never destroyed: their destructors free textures through the game, which may already be
		// gone when the process exits.
		ui::Text &g_text = *new ui::Text;
		ui::PalettePreview *const g_customPreview = new ui::PalettePreview[2];
		ui::PalettePreview *const g_fallbackPreview = new ui::PalettePreview[2];

		std::string entryName(const Pick &pick, int index)
		{
			index = (index % pick.count() + pick.count()) % pick.count();
			if (index < 8)
				return ui::slotName(index) + (pick.mods[index] ? " (mod)" : "");
			return ui::narrow(displayName(pick.lib.palettes[index - 8]));
		}

		// Panel layout (in the 640x480 game frame): right of the game's own palette preview and deck
		// box, ending above the profile-name line. Left: header, 3 list rows, 2 info lines. Right:
		// two small previews, "Custom" (the custom palette) and "Fallback NN" (the slot players
		// without PaletteShare see).
		// Note: the game's text renderer treats '<' as markup, so no angle brackets in strings.
		constexpr float PANEL_X = 132, PANEL_W = 300, PANEL_H = 96;
		constexpr float PANEL_Y[2] = {44, 324};
		constexpr float ROW = 14;
		constexpr float CUSTOM_X = 200, FALLBACK_X = 262;  // preview centers, relative to the panel
		constexpr float PREVIEW_HEIGHT = 75;

		// What the pick's player presses for a game button.
		std::string key(const Pick &pick, ui::Button button)
		{
			return ui::buttonName(pick.keys ? pick.keys : reinterpret_cast<const SokuLib::KeymapManager *>(game::MENU_KEYS), button);
		}

		void drawPick(const Pick &pick, int player)
		{
			const float x = PANEL_X, y = PANEL_Y[player];
			ui::drawBox(x, y, PANEL_W, PANEL_H, 0xB0000000, 0xC0B0B0B0);
			ui::drawBox(x + 170, y + 4, 1, PANEL_H - 8, 0x60FFFFFF, 0);

			// Header and the list: the highlighted entry with one above and one below
			g_text.draw("Palette  " + std::to_string(pick.listedPosition()) + "/" + std::to_string(pick.listedCount()), x + 8, y + 2);
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
				if (custom->modSlot >= 0)  // this game's colors for a slot a palette mod replaces
					g_text.draw("Mod", x + CUSTOM_X - 12, y + 2);
				else if (custom->originalSlot >= 0)
					g_text.draw("Original", x + CUSTOM_X - 22, y + 2);
				else
					g_text.draw("Custom", x + CUSTOM_X - 20, y + 2);
				g_customPreview[player].showPalette(pick.charId, custom->palette, x + CUSTOM_X, y + PANEL_H - 3, PREVIEW_HEIGHT);
				if (custom->originalSlot >= 0) {  // always its own slot: no fallback to show
					g_fallbackPreview[player].clear();
				} else {
					g_text.draw("Fallback 0" + std::to_string(slot), x + FALLBACK_X - 30, y + 2);
					g_fallbackPreview[player].showSlot(pick.charId, slot, x + FALLBACK_X, y + PANEL_H - 3, PREVIEW_HEIGHT);
				}
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
				g_text.draw("Up/Down  " + key(pick, config.fallbackButton) + " save  " + key(pick, ui::Button::B) + " cancel", x + 8, infoY + ROW);
				return;
			}
			bool original = custom->originalSlot >= 0;
			std::string fallback = original ? "Original colors" : "Fallback " + ui::slotName(custom->fallback) + (custom->fallbackIsDefault ? " (default)" : "");
			if (pick.bumped(custom->fallback) != custom->fallback)  // mirror match: this match only
				fallback = ui::slotName(custom->fallback) + " taken, others see 0" + std::to_string(pick.bumped(custom->fallback));
			else if (pick.unreachable == custom->fallback)
				fallback = ui::slotName(custom->fallback) + " can't be reached";
			else if (!original && ui::slotDiffersFromOriginal(pick.charId, custom->fallback))
				// a palette mod replaces the slot in this game; the preview shows what players
				// without palette mods see
				fallback = "Fallback 0" + std::to_string(custom->fallback) + ": original colors";
			g_text.draw(fallback, x + 8, infoY);

			switch (net::peerStatus()) {
			case net::PeerStatus::HasMod: g_text.draw("Opponent sees your palette", x + 8, infoY + ROW); break;
			case net::PeerStatus::NoMod:
			case net::PeerStatus::Incompatible: g_text.draw("Opponent sees " + ui::slotName(pick.target()), x + 8, infoY + ROW); break;
			case net::PeerStatus::Checking: g_text.draw("Opponent: checking...", x + 8, infoY + ROW); break;
			default:
				if (!original && config.fallbackButton != ui::Button::None)
					g_text.draw(key(pick, config.fallbackButton) + ": change fallback", x + 8, infoY + ROW);
				break;
			}
		}

		void rebuildPreview(SokuLib::Select *sel, int player, int charId)
		{
			auto rebuild = reinterpret_cast<void (__fastcall *)(SokuLib::Select *, void *, int, int, int, bool)>(SET_PALETTE_SELECT);
			rebuild(sel, nullptr, player, charId, wheelOf(sel, player).pos, false);
		}

		void refreshPreview(SokuLib::Select *sel, int player, Pick &pick)
		{
			pick.previewDirty = false;
			rebuildPreview(sel, player, pick.charId);
		}

		// The game rebuilds the opponent's preview when their wheel moves (their inputs), but their
		// palette comes on our own channel, usually a moment later, or without any wheel move at all
		// (a custom palette on the slot they're already on). So rebuild it when a new one arrives;
		// the preview hook then shows it. Game thread, without g_mutex (the rebuild calls back in).
		unsigned g_seenGeneration = 0;
		bool g_refreshOpponent = false;  // set by Ctrl+P: rebuild with or without their palette

		void refreshOpponentPreview(SokuLib::Select *sel)
		{
			unsigned generation = net::remoteGeneration();
			if (generation == g_seenGeneration && !g_refreshOpponent)
				return;
			g_seenGeneration = generation;
			g_refreshOpponent = false;
			int local = net::localSide();
			if (!online() || local < 0)
				return;
			int side = 1 - local;
			int charId = characterOf(sel, side);
			if (charId < 0)
				return;
			log("picker: opponent palette arrived, rebuilding side=%d preview (char=%d slot=%d)", side, charId, wheelOf(sel, side).pos);
			rebuildPreview(sel, side, charId);
		}

		void beforeRender(SokuLib::Select *sel)
		{
			logEnvironmentOnce();
			// Ctrl+P works here too: the opponent's preview follows it.
			if (pollOpponentToggle())
				g_refreshOpponent = true;
			pollSaveHotkey(true);
			refreshOpponentPreview(sel);
			if (disabled())
				return;
			for (int side = 0; side < 2; side++) {
				std::unique_lock lock(g_mutex);
				auto &pick = g_picks[side];
				if ((pick.active && pick.previewDirty) || pick.previewAfterConfirm) {
					pick.previewAfterConfirm = false;
					lock.unlock();  // the rebuild calls back into our preview hook
					refreshPreview(sel, side, pick);
				}
			}
		}

		void afterRender(SokuLib::Select *sel)
		{
			drawNotice();
			if (disabled())
				return;
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

		template<int N>
		struct RenderHook {
			static inline int (__fastcall *orig)(void *self) = nullptr;

			static int __fastcall hook(void *self)
			{
				auto sel = static_cast<SokuLib::Select *>(self);
				guarded("picker: before drawing", [&] { beforeRender(sel); });
				int ret = orig(self);
				guarded("picker: drawing", [&] { afterRender(sel); });
				return ret;
			}
		};

		template<int N>
		void hookRender(void *vtable)
		{
			auto slot = reinterpret_cast<DWORD>(vtable) + 8;  // destructor, onProcess, onRender
			RenderHook<N>::orig = patch::pointer(slot, &RenderHook<N>::hook, "picker: select screen drawing");
		}
	}

	std::optional<Palette> previewFor(int player, int charId, int slot)
	{
		if (player < 0 || player > 1)
			return std::nullopt;
		if (disabled())
			return std::nullopt;
		std::lock_guard lock(g_mutex);
		auto &pick = g_picks[player];
		if (pick.active && pick.charId == charId) {
			if (auto custom = pick.custom())
				return custom->palette;
			// A normal slot the wheel is still on its way to: show that slot's colors already, not
			// the ones it passes on the way.
			if (slot != pick.index && pick.slotPalettes[pick.index])
				return pick.slotPalettes[pick.index];
			return std::nullopt;
		}
		if (pick.confirmed && pick.confirmedChar == charId)
			return pick.confirmedCustom;
		return std::nullopt;
	}

	std::optional<Palette> customFor(int player, int charId)
	{
		if (player < 0 || player > 1 || disabled())
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
		orig_setInputs = patch::call(SETINPUTS_CALL, onSetInputs, "picker: input");
		g_origWheelSound = patch::call(WHEEL_SOUND_CALL, reinterpret_cast<const void *>(&wheelSoundStub), "picker: palette step sound");
		hookRender<0>(&SokuLib::VTable_Select);
		hookRender<1>(&SokuLib::VTable_SelectServer);
		hookRender<2>(&SokuLib::VTable_SelectClient);
		log("picker: installed (SetInputs orig=%p)", orig_setInputs);
	}
}
