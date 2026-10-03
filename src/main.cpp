#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <fstream>
#include <filesystem>
#include <string>
#include "hooks.hpp"
#include "log.hpp"
#include "library.hpp"
#include "menu.hpp"
#include "net.hpp"
#include "picker.hpp"
#include "replaymeta.hpp"

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

// Documents\PaletteShare\palettes, if it can be created and written to: outside the game, so
// updating or removing the mod (SokuLauncher deletes the mod's folder) never takes the palettes.
static std::filesystem::path outsideLibrary()
{
	PWSTR documents = nullptr;
	std::filesystem::path folder;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documents)) && documents)
		folder = std::filesystem::path(documents) / L"PaletteShare" / L"palettes";
	CoTaskMemFree(documents);
	if (folder.empty())
		return {};
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
	auto storage = iniString(ini, L"Replay", L"Storage");
	ps::config.replayMetaInRep = _stricmp(storage.c_str(), "rep") == 0;
	if (!storage.empty() && !ps::config.replayMetaInRep && _stricmp(storage.c_str(), "sidecar"))
		ps::log("config: [Replay] Storage=%s is neither sidecar nor rep, using sidecar", storage.c_str());
	ps::config.toggleModifier = iniInt(ini, L"Hotkey", L"Modifier", VK_CONTROL);
	ps::config.toggleKey = iniInt(ini, L"Hotkey", L"Key", 'P');
	ps::config.hotkeyIgnoresFocus = iniInt(ini, L"Debug", L"HotkeyIgnoresFocus", 0) != 0;
	ps::log("config: ShowOpponentPalettes=%d ShowOriginalPalettes=%d hotkey modifier=0x%02X key=0x%02X",
		ps::config.showOpponentPalettes, ps::config.showOriginalPalettes, ps::config.toggleModifier, ps::config.toggleKey);

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
	ps::logOpen(dir / L"PaletteShare.log", GetPrivateProfileIntW(L"Debug", L"Log", 0, ini.c_str()) != 0);
	ps::log("PaletteShare %s starting in %ls", PALETTESHARE_VERSION, dir.c_str());

	ps::cache.setDirectory(dir / L"cache");
	auto library = outsideLibrary();
	if (library.empty()) {
		library = dir / L"palettes";
		ps::log("library: Documents\\PaletteShare isn't usable, keeping palettes in %ls", library.c_str());
	} else {
		ps::log("library: %ls (palettes in %ls are read too)", library.c_str(), (dir / L"palettes").c_str());
	}
	ps::setLibraryRoots(library, dir / L"palettes");
	loadConfig(ini);
	ps::installHooks();
	ps::net::install();
	ps::picker::install();
	ps::menu::install();
	ps::meta::install(ps::config.replayMetaInRep ? ps::meta::Storage::Rep : ps::meta::Storage::Sidecar);
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
