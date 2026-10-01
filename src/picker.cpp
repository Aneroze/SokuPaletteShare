#include "picker.hpp"
#include "hooks.hpp"
#include "library.hpp"
#include "log.hpp"
#include "net.hpp"
#include <windows.h>
#include <SokuLib.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ps::picker {
	namespace {
		constexpr DWORD MENU_KEYS = 0x8986A8;       // the local player's menu KeymapManager
		constexpr DWORD SETINPUTS_CALL = 0x40A45D;  // the game's only call to KeymapManager::SetInputs
		constexpr DWORD SET_PALETTE_SELECT = 0x41FE60;  // rebuilds a player's select preview
		constexpr int KEYINPUT_OFFSET = 0x38;
		constexpr int OUTKEYS_OFFSET = 0x62;        // bits sent in netplay: up 1, down 2, A 16
		constexpr int STAGE_PALETTE = 1;
		constexpr int MAX_STEER_PRESSES = 16;       // then the slot is unreachable (mirror)
		constexpr int STEER_TIMEOUT_FRAMES = 90;    // a press the game never applied

		enum class Steer { Idle, Pressed, Waiting };

		struct Pick {
			bool active = false;     // in the palette step and driven by us
			int charId = -1;
			CharacterLibrary lib;    // snapshot taken when the palette step started
			int index = 0;           // 0..7 vanilla slots, 8.. library palettes
			bool previewDirty = false;
			Steer steer = Steer::Idle;
			int steerDir = 0;
			int wheelAtPress = -1;
			int waitFrames = 0;
			int presses = 0;
			int unreachable = -1;    // fallback slot the wheel couldn't reach (mirror match)
			bool pendingConfirm = false;
			// what was confirmed, kept for the loading screen
			bool confirmed = false;
			int confirmedChar = -1;
			std::optional<Palette> confirmedCustom;

			int count() const { return 8 + static_cast<int>(lib.palettes.size()); }
			const LibraryPalette *custom() const { return index >= 8 ? &lib.palettes[index - 8] : nullptr; }
			int target() const { return index < 8 ? index : lib.palettes[index - 8].fallback; }
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

		// The player our menu keys drive: player 1 offline, our side in netplay.
		int localPlayer()
		{
			if (SokuLib::sceneId == SokuLib::SCENE_SELECT)
				return 0;
			return net::localSide();
		}

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

		void startPick(Pick &pick, int charId, int wheel)
		{
			pick = Pick{};
			pick.active = true;
			pick.charId = charId;
			pick.lib = library(charId);
			pick.index = wheel;
			if (!pick.lib.lastSelected.empty()) {
				for (size_t i = 0; i < pick.lib.palettes.size(); i++)
					if (pick.lib.palettes[i].name == pick.lib.lastSelected)
						pick.index = 8 + static_cast<int>(i);
			} else if (pick.lib.lastSlot >= 0 && pick.lib.lastSlot < 8) {
				pick.index = pick.lib.lastSlot;
			}
			pick.previewDirty = true;
			log("picker: palette step for char=%d, %d custom palette(s), starting at %d", charId, (int)pick.lib.palettes.size(), pick.index);
		}

		// Runs once per frame after the game decoded the local player's keys.
		void drive(SokuLib::KeymapManager *mgr)
		{
			auto sel = selectScene();
			int player = sel ? localPlayer() : -1;
			if (!sel || player < 0) {
				std::lock_guard lock(g_mutex);
				for (auto &p : g_picks) {
					p.active = false;
					if (SokuLib::sceneId == SokuLib::SCENE_TITLE)
						p.confirmed = false;  // a pick never outlives its session
				}
				return;
			}

			std::lock_guard lock(g_mutex);
			auto &pick = g_picks[player];
			int stage = stageOf(sel, player);
			int charId = characterOf(sel, player);
			int wheel = wheelOf(sel, player).pos;

			if (stage != STAGE_PALETTE || charId < 0) {
				if (pick.active && stage < STAGE_PALETTE)
					pick.confirmed = false;  // went back to character choice
				pick.active = false;
				return;
			}
			if (!pick.active || pick.charId != charId)
				startPick(pick, charId, wheel);

			auto &in = *reinterpret_cast<SokuLib::KeyInput *>(reinterpret_cast<char *>(mgr) + KEYINPUT_OFFSET);
			auto &bits = *reinterpret_cast<uint16_t *>(reinterpret_cast<char *>(mgr) + OUTKEYS_OFFSET);

			// 1. Up/Down move our list (with the game's usual hold-to-repeat feel).
			int v = in.verticalAxis;
			int move = (v == 1 || (v > 24 && v % 6 == 0)) ? 1 : (v == -1 || (v < -24 && -v % 6 == 0)) ? -1 : 0;
			if (move) {
				pick.index = (pick.index + move + pick.count()) % pick.count();
				pick.previewDirty = true;
				pick.presses = 0;
				pick.unreachable = -1;
				pick.confirmed = false;
			}
			if (in.a == 1)
				pick.pendingConfirm = true;

			// 2. Steer the game's wheel to the fallback slot, one press at a time: wait until the
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
						log("picker: slot %d can't be reached (taken in a mirror match?), staying on %d", target, wheel);
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
			if (pick.pendingConfirm && settled) {
				emitA = 1;
				pick.pendingConfirm = false;
				pick.confirmed = true;
				pick.confirmedChar = charId;
				auto custom = pick.custom();
				pick.confirmedCustom = custom ? std::optional<Palette>(custom->palette) : std::nullopt;
				rememberSelection(charId, custom ? custom->name : L"", custom ? -1 : pick.index);
				log("picker: confirmed char=%d %ls, game slot %d", charId, custom ? custom->name.c_str() : L"(vanilla)", wheel);
			}

			in.verticalAxis = emitV;
			in.a = emitA;
			bits &= ~(1 | 2 | 16);
			bits |= (emitV < 0 ? 1 : 0) | (emitV > 0 ? 2 : 0) | (emitA ? 16 : 0);
		}

		void __fastcall onSetInputs(void *mgr, void *edx)
		{
			orig_setInputs(mgr, edx);
			if (!g_disabled && reinterpret_cast<DWORD>(mgr) == MENU_KEYS)
				drive(static_cast<SokuLib::KeymapManager *>(mgr));
		}

		// --- overlay ---------------------------------------------------------------------------

		class Text {
		public:
			void draw(const std::string &text, float x, float y)
			{
				if (!_font) {
					_font = std::make_unique<SokuLib::SWRFont>();
					_font->create();
					SokuLib::FontDescription desc{};
					strcpy(desc.faceName, "Tahoma");
					desc.r1 = desc.r2 = desc.g1 = desc.g2 = 255;
					desc.b1 = 250;
					desc.b2 = 120;
					desc.height = 14;
					desc.weight = 400;
					desc.shadow = true;
					_font->setIndirect(desc);
				}
				auto &sprite = _sprites[text];
				if (!sprite) {
					sprite = std::make_unique<SokuLib::Sprite>();
					int tex = 0;
					SokuLib::textureMgr.createTextTexture(&tex, text.c_str(), *_font, 220, 18, nullptr, nullptr);
					sprite->setTexture2(tex, 0, 0, 220, 18);
				}
				sprite->render(x, y);
			}

			void clear()
			{
				for (auto &[_, sprite] : _sprites)
					if (sprite && sprite->dxHandle)
						SokuLib::textureMgr.remove(sprite->dxHandle);
				_sprites.clear();
			}

		private:
			std::unique_ptr<SokuLib::SWRFont> _font;
			std::map<std::string, std::unique_ptr<SokuLib::Sprite>> _sprites;
		};

		Text g_text;

		std::string narrow(const std::wstring &w)
		{
			int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
			std::string s(n > 0 ? n - 1 : 0, '\0');
			WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
			return s;
		}

		std::string slotName(int slot)
		{
			return "Default 0" + std::to_string(slot);
		}

		std::string entryName(const Pick &pick, int index)
		{
			index = (index % pick.count() + pick.count()) % pick.count();
			return index < 8 ? slotName(index) : narrow(pick.lib.palettes[index - 8].name);
		}

		void drawPick(const Pick &pick, int player)
		{
			const float x = 140, y = player == 0 ? 92.f : 372.f;
			SokuLib::MenuCursor::render(x, y, 160);
			for (int d = -2; d <= 2; d++)
				g_text.draw(entryName(pick, pick.index + d), x - 6 * std::abs(d), y + 18 * d);

			float info = y + 18 * 3 + 4;
			g_text.draw(std::to_string(pick.index + 1) + " / " + std::to_string(pick.count()), x + 150, y - 36);
			auto custom = pick.custom();
			if (!custom) {
				g_text.draw("Vanilla palette", x, info);
				return;
			}
			std::string fallback = "Fallback: " + slotName(custom->fallback) + (custom->fallbackIsDefault ? " (default)" : "");
			if (pick.unreachable == custom->fallback)
				fallback = "Fallback " + slotName(custom->fallback) + " taken: others see your slot";
			g_text.draw(fallback, x, info);

			switch (net::peerStatus()) {
			case net::PeerStatus::HasMod: g_text.draw("Opponent sees: " + narrow(custom->name), x, info + 18); break;
			case net::PeerStatus::NoMod:
			case net::PeerStatus::Incompatible: g_text.draw("Opponent sees: " + slotName(custom->fallback), x, info + 18); break;
			case net::PeerStatus::Checking: g_text.draw("Opponent sees: checking...", x, info + 18); break;
			default: break;
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
				int player = localPlayer();
				if (!g_disabled && player >= 0) {
					std::unique_lock lock(g_mutex);
					auto &pick = g_picks[player];
					if (pick.active && pick.previewDirty) {
						lock.unlock();  // the rebuild calls back into our preview hook
						refreshPreview(sel, player, pick);
					}
				}
				int ret = orig(self);
				if (!g_disabled && player >= 0) {
					std::lock_guard lock(g_mutex);
					if (g_picks[player].active)
						drawPick(g_picks[player], player);
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
