#include "hooks.hpp"
#include "game.hpp"
#include "guard.hpp"
#include "patch.hpp"
#include "log.hpp"
#include "net.hpp"
#include "picker.hpp"
#include "original.hpp"
#include "replay.hpp"
#include "saved.hpp"
#include "ui.hpp"
#include <windows.h>
#include <SokuLib.hpp>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// How the game loads a character's palette (th123 1.10a):
//
//   character factory 0x46DE31: call 0x46C0B0   ; thiscall on the new v2::Player, no stack args
//     0x46C0F7: call 0x4679E0                   ; loads the character's .pat, passing
//                                               ; name(characterIndex) and paletteId
//       0x467AEB: sprintf "data/character/%s/palette%03d.bmp" (Okuu may use "...%03db.bmp")
//       0x467B7D: call 0x408BE0                 ; load that file into the global palette 0x896B88
//       0x467B82: (palette ready)               ; palette-picker jumps straight here with its own
//                                               ; palette already written, so we hook this spot
//       0x467C43: call 0x405030 per image       ; TextureManager::loadTexture("data/character/
//                                               ; <name>/<image>"): the 8-bit image goes through
//                                               ; the palette, so the colors are baked in here
//
// Hooking the factory's call tells us WHICH player is loading (v2::Player::teamId).
//
// Character select builds a preview the same way (0x41FE60): palette load at 0x41FFB7 (or
// palette-picker's, which jumps to 0x41FFBC), then the preview sprite is built from it. The
// getCharName call at 0x41FFBD runs with that palette loaded and the player in edi.
//
// Characters are created on the game's loader thread (0x46E6D0 -> 0x46E5C0 -> createPlayer
// 0x46DA40), one at a time, while the hotkey runs on the render thread: g_loadingPlayer is
// per-thread and g_sides is guarded by g_sidesMutex.
//
// Toggling a palette mid-match: the indexed pixels are gone once the textures exist, so we record
// each texture's image path at load and re-create the textures through the same loader with the
// other palette, then swap the D3D textures behind the existing handles. The game keeps using
// its handles; nothing in the game state changes (rollback-safe).

namespace ps {
	Config config;

	namespace {
		struct Side {
			int charId = -1;
			int slot = -1;
			Palette vanilla;                 // what this game shows without PaletteShare
			std::optional<Palette> custom;   // the opponent's (or debug-forced) palette, checked
			bool showingCustom = false;
			std::vector<std::pair<int, std::string>> textures;  // handle, image path
		};

		std::mutex g_sidesMutex;
		Side g_sides[2];
		bool g_showOpponent = true;
		unsigned g_seenGeneration = 0;
		thread_local SokuLib::v2::Player *g_loadingPlayer = nullptr;
		// Character select: what the opponent's preview shows (their custom palette, checked), for
		// the save hotkey. Under g_sidesMutex.
		struct Shown {
			int charId = -1;
			std::optional<Palette> custom;
		};
		Shown g_selectShown[2];

		void (__fastcall *orig_loadCharacter)(SokuLib::v2::Player *self, void *edx) = nullptr;
		int *(__fastcall *orig_loadTexture)(void *mgr, void *edx, int *ret, LPCSTR path, void *w, void *h) = nullptr;
		DWORD orig_getCharName = 0;

		// Whether the picker chose this side's palette here: our side in netplay, both sides offline
		// (player 2, or player 1 picking for the CPU/dummy), none when watching (replays,
		// spectating). The picker itself only answers for sides that went through it.
		bool pickedHere(int side)
		{
			if (SokuLib::subMode == SokuLib::BATTLE_SUBMODE_REPLAY || SokuLib::mainMode == SokuLib::BATTLE_MODE_VSWATCH)
				return false;
			int local = net::localSide();
			return local < 0 || side == local;
		}

		// Spectators run in the replay sub-mode too, but get their palettes live. Told apart by their
		// own scenes (a replay of a watched match plays in the normal ones).
		bool spectating()
		{
			return SokuLib::sceneId == SokuLib::SCENE_LOADINGWATCH || SokuLib::sceneId == SokuLib::SCENE_BATTLEWATCH;
		}
		bool watchingReplay() { return SokuLib::subMode == SokuLib::BATTLE_SUBMODE_REPLAY && !spectating(); }

		int remoteSide()
		{
			int local = net::localSide();
			return local < 0 ? -1 : 1 - local;
		}

		// A side's profile name in netplay (in game::textCodePage(), as the game has it), "" if none.
		std::string profileName(int side)
		{
			auto net = *reinterpret_cast<SokuLib::NetObject **>(SokuLib::ADDR_PNETOBJECT);
			if (!net || side < 0 || side > 1)
				return {};
			const char *name = side == 0 ? net->profile1name : net->profile2name;
			return std::string(name, strnlen(name, sizeof(net->profile1name)));
		}

		// Saves the opponent's custom palette as shown here (see saved.hpp). Not a debug-forced one.
		void saveOpponent(int side, int charId, const Palette &palette, bool notify)
		{
			const char *character = game::characterName(charId);
			if (side != remoteSide() || config.forcePalette[side] || !character)
				return;
			saved::save(character, profileName(side), game::textCodePage(), palette, notify);
		}

		// Okuu's cloak: the inside (color 31) is magenta in her normal palettes, where the game draws
		// something over it (the starfield, presumably), and dark navy in the "b" palettes it loads
		// instead with configex123.ini [etc] limit=1 (0x467B29). That depends on the viewer's own
		// setting, so whatever palette we show on Okuu keeps the viewer's own color there.
		constexpr int OKUU_CLOAK = 31;

		Palette fitToViewer(int charId, const Palette &palette, const Palette &own)
		{
			return charId == game::CHARACTER_UTSUHO ? withColorOf(palette, own, OKUU_CLOAK) : palette;
		}

		// The palette to offer as "custom" for a side: the one the replay being watched recorded, else
		// the debug force, else the opponent's: the one they offered for this slot, or (character
		// select previews, anySlot) the one they show now.
		std::optional<Palette> customFor(int side, int charId, int slot, const Palette &reference, bool anySlot = false)
		{
			std::optional<Palette> palette;
			if (watchingReplay())
				palette = replay::paletteFor(side, charId, slot);
			else if (config.forcePalette[side] && !spectating())
				palette = config.forcePalette[side];
			else if (spectating() || side == remoteSide())
				palette = anySlot ? net::receivedNow(side, charId) : net::received(side, charId, slot);
			if (!palette)
				return std::nullopt;
			auto checked = fitToViewer(charId, lockTransparency(*palette, reference), reference);
			if (hashPalette(checked) == hashPalette(reference))
				return std::nullopt;  // the opponent uses the same palette we'd show anyway
			return checked;
		}

		// Re-creating a side's textures with another palette and swapping them in behind the same
		// handles. A late palette's is spread over a few frames (all at once takes ~30 ms: a visible
		// hitch at a random moment mid-match); the hotkey's happens at once. The new textures are
		// made first, then all swapped in. Render thread, under g_sidesMutex.
		struct Rebuild {
			bool active = false;
			int side = -1;
			Palette palette;
			bool toCustom = false;
			size_t next = 0;
			std::vector<int> fresh;  // one per side.textures entry, 0 when it failed
		};
		// One per side: spectating, both sides' palettes may arrive late at once.
		Rebuild g_rebuilds[2];
		constexpr size_t TEXTURES_PER_FRAME = 96;  // both sides together

		void cancelRebuild(int index)
		{
			auto &rebuild = g_rebuilds[index];
			if (!rebuild.active)
				return;
			for (int handle : rebuild.fresh)
				if (handle)
					SokuLib::textureMgr.remove(handle);
			rebuild = Rebuild{};
		}

		void startRebuild(int index, const Palette &palette, bool toCustom)
		{
			cancelRebuild(index);
			auto &rebuild = g_rebuilds[index];
			rebuild.active = true;
			rebuild.side = index;
			rebuild.palette = palette;
			rebuild.toCustom = toCustom;
		}

		// Returns how many textures it made (out of `budget`).
		size_t stepRebuild(Rebuild &rebuild, size_t budget)
		{
			if (!rebuild.active)
				return 0;
			auto &side = g_sides[rebuild.side];
			auto &current = SokuLib::Palette::currentPalette;
			auto saved = readGamePalette(current);
			writeGamePalette(rebuild.palette, current);
			size_t made = 0;
			for (; made < budget && rebuild.next < side.textures.size(); made++, rebuild.next++) {
				int fresh = 0;
				SokuLib::textureMgr.loadTexture(&fresh, side.textures[rebuild.next].second.c_str(), nullptr, nullptr);
				rebuild.fresh.push_back(fresh);
			}
			if (saved.valid())
				writeGamePalette(saved, current);
			if (rebuild.next < side.textures.size())
				return made;

			int swapped = 0;
			for (size_t i = 0; i < rebuild.fresh.size(); i++) {
				int fresh = rebuild.fresh[i];
				if (!fresh)
					continue;
				auto a = SokuLib::textureMgr.Get(side.textures[i].first);
				auto b = SokuLib::textureMgr.Get(fresh);
				if (a && b && fresh != side.textures[i].first) {
					std::swap(*a, *b);
					swapped++;
				}
				SokuLib::textureMgr.remove(fresh);  // releases the texture we swapped out
			}
			side.showingCustom = rebuild.toCustom;
			log("  side=%d: %d/%d textures swapped in", rebuild.side, swapped, (int)side.textures.size());
			rebuild = Rebuild{};
			return made;
		}

		void stepRebuilds()
		{
			size_t budget = TEXTURES_PER_FRAME;
			for (auto &rebuild : g_rebuilds)
				budget -= (std::min)(budget, stepRebuild(rebuild, budget));
		}

		void showSide(int index, bool custom)
		{
			auto &side = g_sides[index];
			cancelRebuild(index);  // the hotkey wins over a late palette still being prepared
			if (!side.custom || side.showingCustom == custom)
				return;
			startRebuild(index, custom ? *side.custom : side.vanilla, custom);
			stepRebuild(g_rebuilds[index], SIZE_MAX);  // at once: the player asked for it
		}

		// Mirror safety net: the picker keeps both sides of a mirror match apart, but if side 1 would
		// still look exactly like side 0 (a race online), side 1 yields. Same rule on both games.
		bool looksLikeSide0(int index, int charId, const Palette &palette)
		{
			if (index != 1 || g_sides[0].charId != charId)
				return false;
			auto &first = g_sides[0];
			// What side 0 shows, or is about to (both sides' palettes arriving late, spectating).
			bool custom = g_rebuilds[0].active ? g_rebuilds[0].toCustom : first.showingCustom;
			return sameColors(custom && first.custom ? *first.custom : first.vanilla, palette);
		}

		void __fastcall onLoadCharacter(SokuLib::v2::Player *self, void *edx)
		{
			g_loadingPlayer = self;
			orig_loadCharacter(self, edx);
			g_loadingPlayer = nullptr;
		}

		// What a replay saved after this match keeps for a side: the custom palette chosen here, or
		// for a side chosen here, what it shows if that isn't the game's own palette for the slot,
		// whatever made it so (palette-picker, a palette mod, edited data files); the opponent's
		// custom palette is recorded where it's applied.
		void recordForReplay(int index, const Side &side, const std::optional<Palette> &picked)
		{
			if (watchingReplay())
				return;
			std::optional<Palette> shown = picked;
			if (!shown && pickedHere(index) && side.vanilla.valid()) {
				auto orig = original::palette(game::characterName(side.charId), side.slot);
				if (orig && !sameColors(*orig, side.vanilla))
					shown = side.vanilla;
			}
			replay::record(index, side.charId, side.slot, shown, true);
		}

		// The character's palette is in currentPalette (vanilla load or palette-picker).
		void paletteReady()
		{
			auto player = g_loadingPlayer;
			if (!player)
				return;

			int index = player->teamId;
			if (index < 0 || index > 1)
				return;

			std::lock_guard lock(g_sidesMutex);
			auto &palette = SokuLib::Palette::currentPalette;
			auto &side = g_sides[index];
			cancelRebuild(index);
			side = Side{};
			if (!game::normalCharacter(player->characterIndex) || player->paletteId < 0 || player->paletteId > 7)
				return;  // Soku2 and other added characters aren't supported
			side.charId = player->characterIndex;
			side.slot = player->paletteId;
			std::optional<Palette> picked;  // the custom palette chosen here, recorded for replays
			if (pickedHere(index)) {
				if (auto mine = picker::customFor(index, side.charId)) {
					*mine = fitToViewer(side.charId, *mine, readGamePalette(palette));
					if (looksLikeSide0(index, side.charId, *mine)) {
						log("load side=%d: our custom palette is the same as side 0's (mirror), using game slot %d", index, side.slot);
					} else {
						writeGamePalette(*mine, palette);
						picked = std::move(mine);
						log("load side=%d: our picked custom palette (game slot %d is the fallback)", index, side.slot);
					}
				}
			}
			side.vanilla = readGamePalette(palette);
			recordForReplay(index, side, picked);
			if (!side.vanilla.valid()) {
				log("load side=%d char=%d slot=%d: palette unreadable (bpp=%d)", index, side.charId, side.slot, (int)palette.bitsPerPixel);
				return;
			}

			auto hash = hashPalette(side.vanilla);
			log("load side=%d char=%d slot=%d bpp=%d hash=%s", index, side.charId, side.slot, (int)side.vanilla.bpp, hash.hex().c_str());

			if (index == net::localSide())
				net::offerLocal(side.charId, side.slot, side.vanilla, hash, true);
			net::sideLoaded(index, side.charId, side.slot, index == net::localSide() ? &side.vanilla : nullptr);

			side.custom = customFor(index, side.charId, side.slot, side.vanilla);
			if (!side.custom && index == remoteSide())
				log(net::received(index, side.charId, side.slot) ? "  side=%d: the opponent's palette for char=%d slot=%d looks like ours" :
				                                                   "  side=%d: no palette from the opponent for char=%d slot=%d (yet)",
					index, side.charId, side.slot);
			if (side.custom && looksLikeSide0(index, side.charId, *side.custom)) {
				log("  side=%d: custom palette is the same as side 0's (mirror), showing game slot %d", index, side.slot);
				side.custom.reset();
			}
			if (side.custom && !watchingReplay())
				replay::record(index, side.charId, side.slot, side.custom);
			if (side.custom && config.autoSaveOpponents)
				saveOpponent(index, side.charId, *side.custom, false);
			if (side.custom && g_showOpponent) {
				writeGamePalette(*side.custom, palette);
				side.showingCustom = true;
				log("  side=%d: showing custom palette %s", index, hashPalette(*side.custom).hex().c_str());
			} else if (side.custom) {
				log("  side=%d: custom palette available but hidden (toggle is off)", index);
			}
		}

		void __cdecl onPaletteReady()
		{
			guarded("load hook", paletteReady);
		}

		int *__fastcall onLoadTexture(void *mgr, void *edx, int *ret, LPCSTR path, void *w, void *h)
		{
			auto result = orig_loadTexture(mgr, edx, ret, path, w, h);
			if (g_loadingPlayer && ret && *ret && path) {
				int index = g_loadingPlayer->teamId;
				if (index >= 0 && index <= 1) {
					guarded("texture hook", [&] {
						std::lock_guard lock(g_sidesMutex);
						if (g_sides[index].charId >= 0)
							g_sides[index].textures.emplace_back(*ret, path);
					});
				}
			}
			return result;
		}

		void preview(int charId, int playerId, int slot);

		// Character select: currentPalette holds the palette of the preview about to be built.
		void __cdecl onPreview(int charId, int playerId, int slot)
		{
			if (playerId < 0 || playerId > 1 || slot < 0 || slot > 7 || !game::normalCharacter(charId))
				return;  // also: Soku2 and other added characters aren't supported
			guarded("preview hook", [&] { preview(charId, playerId, slot); });
		}

		void preview(int charId, int playerId, int slot)
		{
			int local = net::localSide();
			auto &palette = SokuLib::Palette::currentPalette;

			if (SokuLib::sceneId == SokuLib::SCENE_SELECT || playerId == local) {
				if (auto mine = picker::previewFor(playerId, charId, slot))
					writeGamePalette(fitToViewer(charId, *mine, readGamePalette(palette)), palette);
			}
			if (local < 0)
				return;

			auto current = readGamePalette(palette);
			if (!current.valid())
				return;

			if (playerId == local) {
				net::offerLocal(charId, slot, current, hashPalette(current));
				return;
			}
			auto custom = customFor(playerId, charId, slot, current, true);
			if (custom && g_showOpponent)
				writeGamePalette(*custom, palette);
			std::lock_guard lock(g_sidesMutex);
			g_selectShown[playerId] = Shown{charId, std::move(custom)};
		}

		void pickUpLatePalette(int index);

		// An opponent palette that arrived after their character loaded, or a newer one replacing
		// what we show (e.g. they changed their mind at the last moment): show that one now. When
		// spectating, either side's.
		void pickUpLatePalettes()
		{
			unsigned generation = net::remoteGeneration();
			if (generation == g_seenGeneration)
				return;
			g_seenGeneration = generation;

			if (spectating()) {
				pickUpLatePalette(0);
				pickUpLatePalette(1);
			} else if (remoteSide() >= 0) {
				pickUpLatePalette(remoteSide());
			}
		}

		void pickUpLatePalette(int index)
		{
			std::lock_guard lock(g_sidesMutex);
			auto &side = g_sides[index];
			if (side.charId < 0 || !side.vanilla.valid() || side.textures.empty())
				return;
			auto custom = customFor(index, side.charId, side.slot, side.vanilla);
			if (custom && looksLikeSide0(index, side.charId, *custom))
				custom.reset();
			bool same = custom.has_value() == side.custom.has_value() && (!custom || sameColors(*custom, *side.custom));
			if (same)
				return;
			log("%s palette for side=%d changed after loading: %s", spectating() ? "player" : "opponent", index,
				custom ? hashPalette(*custom).hex().c_str() : "none");
			replay::record(index, side.charId, side.slot, custom);
			bool show = custom && g_showOpponent;
			if (show || side.showingCustom)
				startRebuild(index, show ? *custom : side.vanilla, show);
			if (custom && config.autoSaveOpponents)
				saveOpponent(index, side.charId, *custom, false);
			side.custom = std::move(custom);
		}

		template<int N>
		struct RenderHook {
			static inline int (__fastcall *orig)(void *self) = nullptr;

			static int __fastcall hook(void *self)
			{
				guarded("battle hook", [] {
					logEnvironmentOnce();
					if (pollOpponentToggle()) {
						std::lock_guard lock(g_sidesMutex);
						for (int i = 0; i < 2; i++)
							showSide(i, g_showOpponent);
					}
					pollSaveHotkey(false);
					pickUpLatePalettes();
					std::lock_guard lock(g_sidesMutex);
					stepRebuilds();
				});
				int ret = orig(self);
				guarded("battle drawing", drawNotice);
				return ret;
			}
		};

		template<int N>
		void hookRender(void *vtable)
		{
			auto slot = reinterpret_cast<DWORD>(vtable) + 8;  // destructor, onProcess, onRender
			RenderHook<N>::orig = patch::pointer(slot, &RenderHook<N>::hook, "battle drawing");
		}

		// 0x467B82: xor ebx,ebx / cmp word ptr [esp+28h],bx (7 bytes) -> call stub / nop / nop
		__declspec(naked) void paletteReadyStub()
		{
			__asm {
				pushad
				pushfd
				call onPaletteReady
				popfd
				popad
				xor ebx, ebx
				cmp word ptr [esp + 0x2C], bx  // the original [esp+28h], past our return address
				ret
			}
		}

		// 0x41FFBD: call getCharName(charId). Entry: [esp+4] = charId, [esp+140h] = palette slot
		// (the select function's [esp+138h]), edi = player index.
		__declspec(naked) void previewStub()
		{
			__asm {
				pushad
				push dword ptr [esp + 0x160]  // slot
				push edi                      // playerId
				push dword ptr [esp + 0x2C]   // charId
				call onPreview
				add esp, 12
				popad
				jmp orig_getCharName
			}
		}

		// Hotkey feedback, on screen until g_noticeUntil (set from the save writer's thread too).
		std::mutex g_noticeMutex;
		std::string g_notice[2];  // second line optional
		DWORD g_noticeUntil = 0;

		void notify(const std::string &line1, const std::string &line2 = {})
		{
			std::lock_guard lock(g_noticeMutex);
			g_notice[0] = line1;
			g_notice[1] = line2;
			g_noticeUntil = GetTickCount() + 2000;
		}

		// A hotkey (virtual-key codes, modifier 0 = key alone, key 0 = none) pressed this frame.
		struct Hotkey {
			bool wasDown = false;

			bool pressed(int modifier, int key)
			{
				if (!key)
					return false;
				bool down = (config.hotkeyIgnoresFocus || GetForegroundWindow() == SokuLib::window) &&
				            (GetAsyncKeyState(key) & 0x8000) &&
				            (!modifier || (GetAsyncKeyState(modifier) & 0x8000));
				bool pressed = down && !wasDown;
				wasDown = down;
				return pressed;
			}
		};
	}

	void drawNotice()
	{
		std::string lines[2];
		{
			std::lock_guard lock(g_noticeMutex);
			if (static_cast<int>(g_noticeUntil - GetTickCount()) <= 0)
				return;
			lines[0] = g_notice[0];
			lines[1] = g_notice[1];
		}
		static ui::Text &text = *new ui::Text(14);  // never destroyed (see picker.cpp)
		if (lines[1].empty()) {
			ui::drawBox(232, 92, 176, 22, 0xC0000000, 0xC0B0B0B0);
			text.draw(lines[0], 244, 95);
		} else {
			ui::drawBox(162, 92, 316, 40, 0xC0000000, 0xC0B0B0B0);
			text.draw(lines[0], 170, 95);
			text.draw(lines[1], 170, 112);
		}
	}

	bool pollOpponentToggle()
	{
		static Hotkey hotkey;
		if (!hotkey.pressed(config.toggleModifier, config.toggleKey))
			return false;
		g_showOpponent = !g_showOpponent;
		notify(g_showOpponent ? "Opponent palettes: ON" : "Opponent palettes: OFF");
		log("hotkey: opponent custom palettes %s", g_showOpponent ? "ON" : "OFF");
		SokuLib::playSEWaveBuffer(g_showOpponent ? game::SE_OK : game::SE_BACK);
		return true;
	}

	void pollSaveHotkey(bool select)
	{
		static Hotkey hotkey;
		if (!hotkey.pressed(config.saveModifier, config.saveKey))
			return;
		int side = remoteSide();
		Shown shown;
		if (side >= 0) {
			std::lock_guard lock(g_sidesMutex);
			shown = select ? g_selectShown[side] : Shown{g_sides[side].charId, g_sides[side].custom};
		}
		if (side < 0 || !shown.custom || config.forcePalette[side]) {
			log("hotkey: no opponent custom palette to save");
			notify(side < 0 ? "No opponent online" : "No custom palette to save");
			SokuLib::playSEWaveBuffer(game::SE_BACK);
			return;
		}
		log("hotkey: saving the opponent's palette (char=%d)", shown.charId);
		saveOpponent(side, shown.charId, *shown.custom, true);
		SokuLib::playSEWaveBuffer(game::SE_OK);
	}

	void startSaving(const std::filesystem::path &folder)
	{
		saved::start(folder, [](const saved::Result &result) {
			switch (result.kind) {
			case saved::Result::Saved: notify("Opponent palette saved as", ui::narrow(result.file)); break;
			case saved::Result::AlreadySaved: notify("Already saved as", ui::narrow(result.file)); break;
			case saved::Result::Failed: notify("Could not save the palette", "(see PaletteShare.log)"); break;
			}
		});
	}

	void installHooks()
	{
		g_showOpponent = config.showOpponentPalettes;

		// The load hooks belong together: without the first, the others see no loading player.
		orig_loadCharacter = patch::call(0x46DE31, onLoadCharacter, "character load");
		if (orig_loadCharacter) {
			orig_loadTexture = patch::call(0x467C43, onLoadTexture, "character textures");
			// 0x467B82: xor ebx,ebx (33 DB) / cmp word ptr [esp+28h],bx -> call stub, nop, nop
			static const uint8_t paletteReadyCode[] = {0x33, 0xDB, 0x66, 0x39, 0x5C, 0x24, 0x28};
			patch::callOver(0x467B82, paletteReadyCode, sizeof(paletteReadyCode), reinterpret_cast<const void *>(&paletteReadyStub), "character palette");
		}
		orig_getCharName = patch::call(0x41FFBD, reinterpret_cast<const void *>(&previewStub), "select preview");
		hookRender<0>(&SokuLib::VTable_Battle);
		hookRender<1>(&SokuLib::VTable_BattleServer);
		hookRender<2>(&SokuLib::VTable_BattleClient);
		hookRender<3>(&SokuLib::VTable_BattleWatch);

		log("hooks installed (loadCharacter=%p loadTexture=%p getCharName=%08X)", orig_loadCharacter, orig_loadTexture, orig_getCharName);
	}
}
