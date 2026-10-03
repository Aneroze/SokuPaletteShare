// Windows 7 support. The C++ library of this toolset imports three Windows 8 functions; with them in
// the import table, Windows 7 wouldn't even load the DLL. win7.asm defines those imports
// ourselves (objects win over kernel32.lib), pointing at these stand-ins, which call the real
// function when Windows has it and an older equivalent otherwise.
#include <windows.h>

namespace {
	// CREATEFILE2_EXTENDED_PARAMETERS (fileapi.h only declares it for Windows 8 targets).
	struct CreateFile2Parameters {
		DWORD dwSize;
		DWORD dwFileAttributes;
		DWORD dwFileFlags;
		DWORD dwSecurityQosFlags;
		LPSECURITY_ATTRIBUTES lpSecurityAttributes;
		HANDLE hTemplateFile;
	};

	// COPYFILE2_EXTENDED_PARAMETERS (same story).
	struct CopyFile2Parameters {
		DWORD dwSize;
		DWORD dwCopyFlags;
		BOOL *pfCancel;
		void *pProgressRoutine;
		void *pvCallbackContext;
	};

	FARPROC kernel32(const char *name)
	{
		return GetProcAddress(GetModuleHandleA("kernel32.dll"), name);
	}
}

extern "C" void __stdcall ps_GetSystemTimePreciseAsFileTime(LPFILETIME time)
{
	static auto real = reinterpret_cast<void (WINAPI *)(LPFILETIME)>(kernel32("GetSystemTimePreciseAsFileTime"));
	if (real)
		real(time);
	else
		GetSystemTimeAsFileTime(time);
}

extern "C" HANDLE __stdcall ps_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition, const CreateFile2Parameters *p)
{
	static auto real = reinterpret_cast<HANDLE (WINAPI *)(LPCWSTR, DWORD, DWORD, DWORD, const CreateFile2Parameters *)>(kernel32("CreateFile2"));
	if (real)
		return real(name, access, share, disposition, p);
	DWORD flags = p ? p->dwFileAttributes | p->dwFileFlags | p->dwSecurityQosFlags : 0;
	return CreateFileW(name, access, share, p ? p->lpSecurityAttributes : nullptr, disposition, flags, p ? p->hTemplateFile : nullptr);
}

extern "C" HRESULT __stdcall ps_CopyFile2(LPCWSTR from, LPCWSTR to, const CopyFile2Parameters *p)
{
	static auto real = reinterpret_cast<HRESULT (WINAPI *)(LPCWSTR, LPCWSTR, const CopyFile2Parameters *)>(kernel32("CopyFile2"));
	if (real)
		return real(from, to, p);
	// The flags Windows 7's CopyFileExW knows (fail if exists, restartable, open source for write,
	// allow decrypted destination, copy symlink, no buffering); no progress callback is needed here.
	DWORD flags = p ? p->dwCopyFlags & 0x180F : 0;
	if (CopyFileExW(from, to, nullptr, nullptr, p ? p->pfCancel : nullptr, flags))
		return S_OK;
	return HRESULT_FROM_WIN32(GetLastError());
}
