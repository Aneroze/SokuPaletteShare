#include <windows.h>
#include <fstream>
#include <filesystem>
#include <string>
#include "guard.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "backup.hpp"
#include "library.hpp"
#include "menu.hpp"
#include "net.hpp"
#include "picker.hpp"
#include "replaymeta.hpp"
#include "protocol.hpp"
#include "sha256.hpp"

// th123.exe 1.10a
static const BYTE TARGET_HASH[16] = {0xdf, 0x35, 0xd1, 0xfb, 0xc7, 0xb5, 0x83, 0x31, 0x7a, 0xda, 0xbe, 0x8c, 0xd9, 0xf5, 0x3b, 0x2e};

static std::filesystem::path modulePath(HMODULE module)
{
	std::wstring buffer(MAX_PATH, L'\0');
	DWORD len;
	while ((len = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()))) == buffer.size())
		buffer.resize(buffer.size() * 2);
	buffer.resize(len);
	return std::filesystem::path(buffer).parent_path();
}

// <game folder>\PaletteShare (next to modules\), if it can be created and written to: the palette
// backup goes there, outside the mod's folder (SokuLauncher deletes modules\PaletteShare when the
// mod is removed) yet inside the game folder, so it travels with it (Windows and Wine/Proton alike).
static std::filesystem::path gameFolderData()
{
	auto folder = modulePath(nullptr) / L"PaletteShare";
	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	auto probe = folder / L".write-test";
	{
		std::ofstream out(probe);
		if (!(out << "ok"))
			return {};
	}
	std::filesystem::remove(probe, ec);
	return folder;
}

static std::string iniString(const std::filesystem::path &ini, const wchar_t *section, const wchar_t *key)
{
	wchar_t buffer[256];
	GetPrivateProfileStringW(section, key, L"", buffer, 256, ini.c_str());
	std::string out;
	for (wchar_t *c = buffer; *c; c++)
		out += static_cast<char>(*c);
	return out;
}

static int iniInt(const std::filesystem::path &ini, const wchar_t *section, const wchar_t *key, int fallback)
{
	auto value = iniString(ini, section, key);
	if (value.empty())
		return fallback;
	try {
		return std::stoi(value, nullptr, 0);  // accepts decimal or 0x-prefixed hex
	} catch (...) {
		ps::log("config: [%ls] %ls=%s is not a number, using %d", section, key, value.c_str(), fallback);
		return fallback;
	}
}

static void loadConfig(const std::filesystem::path &ini)
{
	ps::config.showOpponentPalettes = iniInt(ini, L"Display", L"ShowOpponentPalettes", 1) != 0;
	ps::config.showOriginalPalettes = iniInt(ini, L"Picker", L"ShowOriginalPalettes", 0) != 0;
	// A key or modifier taken out of the ini (or left empty) is none: no Key, no hotkey. Without an
	// ini at all, the defaults (Ctrl+P).
	std::error_code ec;
	if (std::filesystem::exists(ini, ec)) {
		auto vk = [&](const wchar_t *key) {
			int value = iniInt(ini, L"Hotkey", key, 0);
			if (value < 0 || value > 0xFE) {
				ps::log("config: [Hotkey] %ls=%d is not a virtual-key code, using none", key, value);
				return 0;
			}
			return value;
		};
		ps::config.toggleModifier = vk(L"Modifier");
		ps::config.toggleKey = vk(L"Key");
	}
	ps::config.hotkeyIgnoresFocus = iniInt(ini, L"Debug", L"HotkeyIgnoresFocus", 0) != 0;
	ps::log("config: ShowOpponentPalettes=%d ShowOriginalPalettes=%d hotkey modifier=0x%02X key=0x%02X%s",
		ps::config.showOpponentPalettes, ps::config.showOriginalPalettes, ps::config.toggleModifier, ps::config.toggleKey,
		ps::config.toggleKey ? "" : " (no hotkey)");

	const wchar_t *keys[2] = {L"ForcePaletteLeft", L"ForcePaletteRight"};
	for (int side = 0; side < 2; side++) {
		auto value = iniString(ini, L"Debug", keys[side]);
		if (value.empty())
			continue;
		ps::config.forcePalette[side] = ps::PaletteHash::fromHex(value);
		if (!ps::config.forcePalette[side])
			ps::log("config: [Debug] %ls is not a 32-digit hex hash, ignored", keys[side]);
	}
}

extern "C" __declspec(dllexport) bool CheckVersion(const BYTE hash[16])
{
	return memcmp(TARGET_HASH, hash, sizeof(TARGET_HASH)) == 0;
}

extern "C" __declspec(dllexport) bool Initialize(HMODULE hMyModule, HMODULE)
{
	static bool initialized = false;
	if (initialized)
		return false;
	initialized = true;

	auto dir = modulePath(hMyModule);
	auto ini = dir / L"PaletteShare.ini";
	ps::logOpen(dir / L"PaletteShare.log", GetPrivateProfileIntW(L"Debug", L"Log", 1, ini.c_str()) != 0);
	ps::log("PaletteShare %s (protocol v%d-v%d) starting in %ls", PALETTESHARE_VERSION, ps::proto::MIN_VERSION, ps::proto::VERSION, dir.c_str());
	if (!ps::sha256SelfTest()) {
		// Every palette would get the same hash: rather not run at all.
		ps::log("SHA-256 self-test failed: PaletteShare is off");
		return false;
	}

	ps::cache.setDirectory(dir / L"cache");
	auto library = dir / L"palettes";
	auto data = gameFolderData();
	ps::log("library: %ls, backup: %ls", library.c_str(), data.empty() ? L"none (the game folder isn't writable)" : (data / L"backup").c_str());
	ps::setLibraryRoot(library);
	ps::guarded("palette backup", [&] {
		ps::backup::start(library, data.empty() ? std::filesystem::path() : data / L"backup", modulePath(nullptr) / L"PaletteShare" / L"palettes");
	});
	loadConfig(ini);
	ps::installHooks();
	ps::net::install();
	ps::picker::install();
	ps::menu::install();
	ps::meta::install();
	return true;
}

extern "C" __declspec(dllexport) void AtExit()
{
	ps::net::shutdown();
	ps::log("PaletteShare stopping");
}

BOOL WINAPI DllMain(HMODULE, DWORD, LPVOID)
{
	return TRUE;
}
