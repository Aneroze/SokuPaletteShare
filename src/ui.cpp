#include "ui.hpp"
#include <d3d9.h>
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace ps::ui {
	namespace {
		constexpr DWORD LOAD_PALETTE_FILE = 0x408BE0;  // thiscall(0x8A0048): (path, Palette *, bpp)
		constexpr DWORD PALETTE_LOADER = 0x8A0048;
	}

	Text::Text(int height) : _height(height) {}

	Text::~Text()
	{
		for (auto &[_, sprite] : _sprites)
			if (sprite && sprite->dxHandle)
				SokuLib::textureMgr.remove(sprite->dxHandle);
		if (_font)
			_font->destruct();
	}

	void Text::draw(const std::string &text, float x, float y)
	{
		if (!_font) {
			_font = std::make_unique<SokuLib::SWRFont>();
			_font->create();
			SokuLib::FontDescription desc{};
			strcpy(desc.faceName, "Tahoma");
			desc.r1 = desc.r2 = desc.g1 = desc.g2 = 255;
			desc.b1 = 250;
			desc.b2 = 120;
			desc.height = _height;
			desc.weight = 400;
			desc.shadow = true;
			_font->setIndirect(desc);
		}
		auto &sprite = _sprites[text];
		if (!sprite) {
			sprite = std::make_unique<SokuLib::Sprite>();
			int tex = 0;
			SokuLib::textureMgr.createTextTexture(&tex, text.c_str(), *_font, 300, _height + 4, nullptr, nullptr);
			sprite->setTexture2(tex, 0, 0, 300, _height + 4);
		}
		sprite->render(x, y);
	}

	void drawBox(float x, float y, float w, float h, DWORD fill, DWORD line)
	{
		struct Vertex { float x, y, z, rhw; DWORD color; };
		IDirect3DDevice9 *dev = SokuLib::pd3dDev;
		if (!dev)
			return;

		DWORD fvf, colorOp, colorArg, alphaOp, alphaArg;
		IDirect3DBaseTexture9 *texture = nullptr;
		dev->GetFVF(&fvf);
		dev->GetTexture(0, &texture);
		dev->GetTextureStageState(0, D3DTSS_COLOROP, &colorOp);
		dev->GetTextureStageState(0, D3DTSS_COLORARG1, &colorArg);
		dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &alphaOp);
		dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &alphaArg);

		dev->SetTexture(0, nullptr);
		dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

		const float r = x + w, b = y + h;
		Vertex quad[4] = {{x, y, 0, 1, fill}, {r, y, 0, 1, fill}, {x, b, 0, 1, fill}, {r, b, 0, 1, fill}};
		dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
		if (line >> 24) {
			Vertex border[5] = {{x, y, 0, 1, line}, {r, y, 0, 1, line}, {r, b, 0, 1, line}, {x, b, 0, 1, line}, {x, y, 0, 1, line}};
			dev->DrawPrimitiveUP(D3DPT_LINESTRIP, 4, border, sizeof(Vertex));
		}

		dev->SetTextureStageState(0, D3DTSS_COLOROP, colorOp);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, colorArg);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, alphaOp);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, alphaArg);
		dev->SetFVF(fvf);
		dev->SetTexture(0, texture);
		if (texture)
			texture->Release();
	}

	void PalettePreview::showSlot(int charId, int slot, float x, float y, float maxHeight)
	{
		show(charId, "slot" + std::to_string(slot), nullptr, slot, x, y, maxHeight);
	}

	void PalettePreview::showPalette(int charId, const Palette &palette, float x, float y, float maxHeight)
	{
		show(charId, hashPalette(palette).hex(), &palette, -1, x, y, maxHeight);
	}

	void PalettePreview::show(int charId, const std::string &key, const Palette *palette, int slot, float x, float y, float maxHeight)
	{
		if (!_manager)
			_manager = std::make_unique<SokuLib::v2::EffectManager_Select>();

		if (charId != _charId || key != _key) {
			clear();
			const char *name = internalName(charId);
			if (!name || !*name)
				return;

			// Build the sprite while the global palette holds the palette we want, then put back
			// whatever was there.
			auto &current = SokuLib::Palette::currentPalette;
			auto saved = readGamePalette(current);
			char path[96];
			if (palette) {
				writeGamePalette(*palette, current);
			} else {
				sprintf(path, "data/character/%s/palette%03d.bmp", name, slot);
				reinterpret_cast<void (__fastcall *)(DWORD, void *, const char *, SokuLib::Palette *, int)>(LOAD_PALETTE_FILE)(PALETTE_LOADER, nullptr, path, &current, 16);
			}
			sprintf(path, "data/scene/select/character/%s/stand.pat", name);
			_manager->LoadPattern(path, 0);
			_sprite = _manager->CreateEffect(0, x, y, SokuLib::Direction::RIGHT, 0, 0);
			if (_sprite)
				_sprite->setPose(0);
			if (saved.valid())
				writeGamePalette(saved, current);
			_charId = charId;
			_key = key;
			_x = x;
			_y = y;
		}
		if (!_sprite)
			return;
		_manager->Update();
		// The select-screen effect renderer ignores renderInfos.scale, so draw the current pose
		// ourselves: start from its base coordinates (relative to the sprite's anchor), scale,
		// move into place.
		// Fit the pose into maxHeight and place it by its bottom center at (x, y), measuring the
		// actual quad, since each character's sprite is offset differently.
		auto &sprite = _sprite->sprite;
		auto bounds = [&](float &minX, float &maxX, float &minY, float &maxY) {
			minX = minY = 1e9f;
			maxX = maxY = -1e9f;
			for (auto &c : sprite.transfCoords) {
				minX = std::min(minX, c.x); maxX = std::max(maxX, c.x);
				minY = std::min(minY, c.y); maxY = std::max(maxY, c.y);
			}
		};
		float minX, maxX, minY, maxY;
		sprite.loadTransform();
		bounds(minX, maxX, minY, maxY);
		float fit = maxY > minY ? std::min(1.f, maxHeight / (maxY - minY)) : 1.f;
		sprite.scaleX(fit);
		sprite.scaleY(fit);
		bounds(minX, maxX, minY, maxY);
		sprite.translate(x - (minX + maxX) / 2, y - maxY, 0);
		sprite.render();
	}

	void PalettePreview::clear()
	{
		if (_manager)
			_manager->ClearPattern();
		_sprite = nullptr;
		_charId = -1;
		_key.clear();
	}

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

	std::string buttonName(const SokuLib::KeymapManager *keys, Button button)
	{
		static const char *const defaults[] = {"Z", "X", "C", "A"};  // the game's default keyboard keys
		const int i = static_cast<int>(button);
		if (!keys)
			return defaults[i];
		const int code = (&keys->bindings.a)[i];
		// The game reads the device index as a signed byte: -1 = keyboard, 0.. = gamepad.
		if (static_cast<signed char>(keys->bindings.index) >= 0)
			return code >= 0 ? "Pad" + std::to_string(code) : defaults[i];  // the game's own "Pad0" naming
		if (code <= 0 || code > 0xFF)
			return defaults[i];
		// DirectInput key codes are scan codes, with 0x80 marking the extended keys.
		char name[32];
		int n = GetKeyNameTextA(((code & 0x7F) << 16) | (code & 0x80 ? 1 << 24 : 0), name, sizeof(name));
		if (n <= 0)
			return defaults[i];
		for (int k = 0; k < n; k++)
			if (name[k] < 0x20 || name[k] > 0x7E || name[k] == '<')  // non-ASCII, or the renderer's markup
				return "Key " + std::to_string(code);
		return std::string(name, n);
	}

	const char *internalName(int charId)
	{
		return reinterpret_cast<const char *(*)(int)>(0x43F3F0)(charId);
	}
}
