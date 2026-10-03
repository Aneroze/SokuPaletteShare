#include "patch.hpp"
#include "log.hpp"
#include <cstring>

namespace ps::patch {
	namespace {
		bool executable(DWORD address)
		{
			MEMORY_BASIC_INFORMATION info;
			if (!address || !VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) || info.State != MEM_COMMIT)
				return false;
			return info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
		}

		// Writes `size` bytes at `at`, whatever its protection.
		void write(DWORD at, const void *bytes, size_t size)
		{
			DWORD old;
			VirtualProtect(reinterpret_cast<LPVOID>(at), size, PAGE_EXECUTE_READWRITE, &old);
			memcpy(reinterpret_cast<void *>(at), bytes, size);
			VirtualProtect(reinterpret_cast<LPVOID>(at), size, old, &old);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), size);
		}

		void jumpOrCall(uint8_t *out, uint8_t opcode, DWORD from, DWORD to)
		{
			out[0] = opcode;
			DWORD rel = to - (from + 5);
			memcpy(out + 1, &rel, 4);
		}

		void skipped(const char *what, DWORD at, const char *why)
		{
			log("patch: %s at %08X skipped: %s (another mod changed it?); that feature is off", what, at, why);
		}
	}

	DWORD call(DWORD at, const void *hook, const char *what)
	{
		auto code = reinterpret_cast<const uint8_t *>(at);
		if (code[0] != 0xE8) {
			skipped(what, at, "not a call");
			return 0;
		}
		int32_t rel;
		memcpy(&rel, code + 1, 4);
		DWORD previous = at + 5 + rel;
		if (!executable(previous)) {
			skipped(what, at, "the call goes nowhere");
			return 0;
		}
		uint8_t bytes[5];
		jumpOrCall(bytes, 0xE8, at, reinterpret_cast<DWORD>(hook));
		write(at, bytes, 5);
		return previous;
	}

	bool callOver(DWORD at, const uint8_t *expected, size_t size, const void *hook, const char *what)
	{
		if (size < 5 || memcmp(reinterpret_cast<const void *>(at), expected, size) != 0) {
			skipped(what, at, "unexpected code");
			return false;
		}
		uint8_t bytes[32];
		jumpOrCall(bytes, 0xE8, at, reinterpret_cast<DWORD>(hook));
		memset(bytes + 5, 0x90, size - 5);
		write(at, bytes, size);
		return true;
	}

	void *entry(DWORD at, const void *hook, const char *what)
	{
		auto code = reinterpret_cast<const uint8_t *>(at);
		bool prologue = code[0] == 0x6A && code[1] == 0xFF && code[2] == 0x68;  // push -1; push imm32
		DWORD chained = 0;
		if (!prologue) {
			if (code[0] != 0xE9) {
				skipped(what, at, "unexpected code");
				return nullptr;
			}
			int32_t rel;
			memcpy(&rel, code + 1, 4);
			chained = at + 5 + rel;
			if (!executable(chained)) {
				skipped(what, at, "the jump goes nowhere");
				return nullptr;
			}
		}
		auto tramp = static_cast<uint8_t *>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!tramp)
			return nullptr;
		if (prologue) {  // the moved prologue, then back into the function
			memcpy(tramp, code, 7);
			jumpOrCall(tramp + 7, 0xE9, reinterpret_cast<DWORD>(tramp + 7), at + 7);
		} else {  // on into the other mod's hook
			jumpOrCall(tramp, 0xE9, reinterpret_cast<DWORD>(tramp), chained);
		}
		FlushInstructionCache(GetCurrentProcess(), tramp, 16);
		uint8_t bytes[7];
		jumpOrCall(bytes, 0xE9, at, reinterpret_cast<DWORD>(hook));
		bytes[5] = bytes[6] = 0x90;
		write(at, bytes, prologue ? 7 : 5);
		return tramp;
	}

	DWORD pointer(DWORD slot, const void *value, const char *what)
	{
		DWORD previous = *reinterpret_cast<const DWORD *>(slot);
		if (!executable(previous)) {
			skipped(what, slot, "the slot doesn't point at code");
			return 0;
		}
		DWORD v = reinterpret_cast<DWORD>(value);
		write(slot, &v, 4);
		return previous;
	}
}
