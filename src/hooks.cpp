#include "hooks.hpp"
#include "log.hpp"
#include "net.hpp"
#include "picker.hpp"
#include "replay.hpp"
#include <windows.h>
#include <SokuLib.hpp>
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
	PaletteCache cache;

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

		bool watchingReplay() { return SokuLib::subMode == SokuLib::BATTLE_SUBMODE_REPLAY; }

		int remoteSide()
		{
			int local = net::localSide();
			return local < 0 ? -1 : 1 - local;
		}

		// The palette to offer as "custom" for a side: the one the replay being watched recorded, else
		// the debug force, else the opponent's: the one they offered for this slot, or (character
		// select previews, anySlot) the one they show now.
		std::optional<Palette> customFor(int side, int charId, int slot, const Palette &reference, bool anySlot = false)
		{
			std::optional<PaletteHash> hash;
			std::optional<Palette> palette;
			if (watchingReplay())
				palette = replay::paletteFor(side, charId, slot);
			else if (config.forcePalette[side])
				hash = config.forcePalette[side];
			else if (side == remoteSide())
				hash = anySlot ? net::remoteShown(charId) : net::remotePalette(charId, slot);
			if (hash) {
				palette = cache.load(*hash);
				if (!palette)
					log("  side=%d: palette %s missing from the cache", side, hash->hex().c_str());
			}
			if (!palette)
				return std::nullopt;
			auto checked = lockTransparency(*palette, reference);
			if (hashPalette(checked) == hashPalette(reference))
				return std::nullopt;  // the opponent uses the same palette we'd show anyway
			return checked;
		}

		// Re-create the side's textures with `palette` and swap them in behind the same handles.
		void rebuildTextures(Side &side, const Palette &palette)
		{
			auto &current = SokuLib::Palette::currentPalette;
			auto saved = readGamePalette(current);
			writeGamePalette(palette, current);

			auto start = GetTickCount64();
			int swapped = 0;
			for (auto &[handle, path] : side.textures) {
				int fresh = 0;
				SokuLib::textureMgr.loadTexture(&fresh, path.c_str(), nullptr, nullptr);
				if (!fresh)
					continue;
				auto a = SokuLib::textureMgr.Get(handle);
				auto b = SokuLib::textureMgr.Get(fresh);
				if (a && b && fresh != handle) {
					std::swap(*a, *b);
					swapped++;
				}
				SokuLib::textureMgr.remove(fresh);  // releases the texture we swapped out
			}

			if (saved.valid())
				writeGamePalette(saved, current);
			log("  rebuilt %d/%d textures in %llu ms", swapped, (int)side.textures.size(), GetTickCount64() - start);
		}

		void showSide(int index, bool custom)
		{
			auto &side = g_sides[index];
			if (!side.custom || side.showingCustom == custom)
				return;
			rebuildTextures(side, custom ? *side.custom : side.vanilla);
			side.showingCustom = custom;
		}

		// Mirror safety net: the picker keeps both sides of a mirror match apart, but if side 1 would
		// still look exactly like side 0 (a race online), side 1 yields. Same rule on both games.
		bool looksLikeSide0(int index, int charId, const Palette &palette)
		{
			if (index != 1 || g_sides[0].charId != charId)
				return false;
			auto &first = g_sides[0];
			return sameColors(first.showingCustom && first.custom ? *first.custom : first.vanilla, palette);
		}

		void __fastcall onLoadCharacter(SokuLib::v2::Player *self, void *edx)
		{
			g_loadingPlayer = self;
			orig_loadCharacter(self, edx);
			g_loadingPlayer = nullptr;
		}

		// The character's palette is in currentPalette (vanilla load or palette-picker).
		void __cdecl onPaletteReady()
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
			side = Side{};
			side.charId = player->characterIndex;
			side.slot = player->paletteId;
			std::optional<Palette> picked;  // the custom palette chosen here, recorded for replays
			if (pickedHere(index)) {
				if (auto mine = picker::customFor(index, side.charId)) {
					if (looksLikeSide0(index, side.charId, *mine)) {
						log("load side=%d: our custom palette is the same as side 0's (mirror), using game slot %d", index, side.slot);
					} else {
						writeGamePalette(*mine, palette);
						picked = std::move(mine);
						log("load side=%d: our picked custom palette (game slot %d is the fallback)", index, side.slot);
					}
				}
			}
			if (!watchingReplay())
				replay::record(index, side.charId, side.slot, picked);
			side.vanilla = readGamePalette(palette);
			if (!side.vanilla.valid()) {
				log("load side=%d char=%d slot=%d: palette unreadable (bpp=%d)", index, side.charId, side.slot, (int)palette.bitsPerPixel);
				return;
			}

			auto hash = hashPalette(side.vanilla);
			cache.store(side.vanilla, hash);
			log("load side=%d char=%d slot=%d bpp=%d hash=%s", index, side.charId, side.slot, (int)side.vanilla.bpp, hash.hex().c_str());

			if (index == net::localSide())
				net::offerLocal(side.charId, side.slot, side.vanilla, hash);

			side.custom = customFor(index, side.charId, side.slot, side.vanilla);
			if (!side.custom && index == remoteSide())
				log("  side=%d: no palette from the opponent for char=%d slot=%d (yet)", index, side.charId, side.slot);
			if (side.custom && looksLikeSide0(index, side.charId, *side.custom)) {
				log("  side=%d: custom palette is the same as side 0's (mirror), showing game slot %d", index, side.slot);
				side.custom.reset();
			}
			if (side.custom && !watchingReplay())
				replay::record(index, side.charId, side.slot, side.custom);
			if (side.custom && g_showOpponent) {
				writeGamePalette(*side.custom, palette);
				side.showingCustom = true;
				log("  side=%d: showing custom palette %s", index, hashPalette(*side.custom).hex().c_str());
			} else if (side.custom) {
				log("  side=%d: custom palette available but hidden (toggle is off)", index);
			}
		}

		int *__fastcall onLoadTexture(void *mgr, void *edx, int *ret, LPCSTR path, void *w, void *h)
		{
			auto result = orig_loadTexture(mgr, edx, ret, path, w, h);
			if (g_loadingPlayer && ret && *ret && path) {
				int index = g_loadingPlayer->teamId;
				if (index >= 0 && index <= 1) {
					std::lock_guard lock(g_sidesMutex);
					g_sides[index].textures.emplace_back(*ret, path);
				}
			}
			return result;
		}

		// Character select: currentPalette holds the palette of the preview about to be built.
		void __cdecl onPreview(int charId, int playerId, int slot)
		{
			if (playerId < 0 || playerId > 1)
				return;
			int local = net::localSide();
			auto &palette = SokuLib::Palette::currentPalette;

			if (SokuLib::sceneId == SokuLib::SCENE_SELECT || playerId == local) {
				if (auto mine = picker::previewFor(playerId, charId, slot))
					writeGamePalette(*mine, palette);
			}
			if (local < 0)
				return;

			auto current = readGamePalette(palette);
			if (!current.valid())
				return;

			if (playerId == local) {
				auto hash = hashPalette(current);
				cache.store(current, hash);
				net::offerLocal(charId, slot, current, hash);
			} else if (g_showOpponent) {
				if (auto custom = customFor(playerId, charId, slot, current, true))
					writeGamePalette(*custom, palette);
			}
		}

		void pollHotkey()
		{
			static bool wasDown = false;
			bool down = (config.hotkeyIgnoresFocus || GetForegroundWindow() == SokuLib::window) &&
			            (GetAsyncKeyState(config.toggleKey) & 0x8000) &&
			            (!config.toggleModifier || (GetAsyncKeyState(config.toggleModifier) & 0x8000));
			if (down && !wasDown) {
				g_showOpponent = !g_showOpponent;
				log("hotkey: opponent custom palettes %s", g_showOpponent ? "ON" : "OFF");
				std::lock_guard lock(g_sidesMutex);
				for (int i = 0; i < 2; i++)
					showSide(i, g_showOpponent);
				SokuLib::playSEWaveBuffer(g_showOpponent ? 0x28 : 0x29);
			}
			wasDown = down;
		}

		// An opponent palette that arrived after their character loaded: show it now.
		void pickUpLatePalettes()
		{
			unsigned generation = net::remoteGeneration();
			if (generation == g_seenGeneration)
				return;
			g_seenGeneration = generation;

			int index = remoteSide();
			if (index < 0)
				return;
			std::lock_guard lock(g_sidesMutex);
			auto &side = g_sides[index];
			if (side.custom || side.charId < 0 || !side.vanilla.valid() || side.textures.empty())
				return;
			side.custom = customFor(index, side.charId, side.slot, side.vanilla);
			if (side.custom)
				replay::record(index, side.charId, side.slot, side.custom);
			if (side.custom && g_showOpponent) {
				log("late palette for side=%d, applying", index);
				showSide(index, true);
			}
		}

		template<int N>
		struct RenderHook {
			static inline int (__fastcall *orig)(void *self) = nullptr;

			static int __fastcall hook(void *self)
			{
				pollHotkey();
				pickUpLatePalettes();
				return orig(self);
			}
		};

		template<int N>
		void hookRender(void *vtable)
		{
			auto slot = reinterpret_cast<DWORD *>(vtable) + 2;  // destructor, onProcess, onRender
			RenderHook<N>::orig = reinterpret_cast<int (__fastcall *)(void *)>(*slot);
			*slot = reinterpret_cast<DWORD>(&RenderHook<N>::hook);
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

		void writeCall(DWORD at, void *target, int nops)
		{
			*reinterpret_cast<BYTE *>(at) = 0xE8;
			*reinterpret_cast<DWORD *>(at + 1) = reinterpret_cast<DWORD>(target) - (at + 5);
			for (int i = 0; i < nops; i++)
				*reinterpret_cast<BYTE *>(at + 5 + i) = 0x90;
		}
	}

	void installHooks()
	{
		g_showOpponent = config.showOpponentPalettes;

		DWORD old;
		VirtualProtect(reinterpret_cast<LPVOID>(TEXT_SECTION_OFFSET), TEXT_SECTION_SIZE, PAGE_EXECUTE_WRITECOPY, &old);
		orig_loadCharacter = SokuLib::TamperNearCall(0x46DE31, onLoadCharacter);
		orig_loadTexture = SokuLib::TamperNearCall(0x467C43, onLoadTexture);
		orig_getCharName = SokuLib::union_cast<DWORD>(SokuLib::TamperNearCall(0x41FFBD, previewStub));
		writeCall(0x467B82, paletteReadyStub, 2);
		VirtualProtect(reinterpret_cast<LPVOID>(TEXT_SECTION_OFFSET), TEXT_SECTION_SIZE, old, &old);

		VirtualProtect(reinterpret_cast<LPVOID>(RDATA_SECTION_OFFSET), RDATA_SECTION_SIZE, PAGE_READWRITE, &old);
		hookRender<0>(&SokuLib::VTable_Battle);
		hookRender<1>(&SokuLib::VTable_BattleServer);
		hookRender<2>(&SokuLib::VTable_BattleClient);
		hookRender<3>(&SokuLib::VTable_BattleWatch);
		VirtualProtect(reinterpret_cast<LPVOID>(RDATA_SECTION_OFFSET), RDATA_SECTION_SIZE, old, &old);
		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);

		log("hooks installed (loadCharacter=%p loadTexture=%p getCharName=%08X)", orig_loadCharacter, orig_loadTexture, orig_getCharName);
	}
}
