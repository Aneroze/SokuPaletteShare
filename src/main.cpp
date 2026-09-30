#include <windows.h>
#include <filesystem>
#include <string>
#include "hooks.hpp"
#include "log.hpp"

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

static std::string iniString(const std::filesystem::path &ini, const wchar_t *section, const wchar_t *key)
{
	wchar_t buffer[256];
	GetPrivateProfileStringW(section, key, L"", buffer, 256, ini.c_str());
	std::string out;
	for (wchar_t *c = buffer; *c; c++)
		out += static_cast<char>(*c);
	return out;
}

static void loadConfig(const std::filesystem::path &ini)
{
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
	ps::log("PaletteShare %s starting in %ls", PALETTESHARE_VERSION, dir.c_str());

	ps::cache.setDirectory(dir / L"cache");
	loadConfig(ini);
	ps::installHooks();
	return true;
}

extern "C" __declspec(dllexport) void AtExit() {}

BOOL WINAPI DllMain(HMODULE, DWORD, LPVOID)
{
	return TRUE;
}
