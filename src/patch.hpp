#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

// Patching the game's code and tables. Every patch first checks what it replaces: a spot another
// mod already changed in a way we can chain through (its own call or jump, a pointer to its code)
// is chained; anything else is left alone and logged, and only that feature of ours stays off,
// instead of breaking the other mod or the game.
namespace ps::patch {
	// A near call (E8 rel32) at `at` now calls `hook`. Returns the previous target, which the hook
	// calls on (it may be another mod's hook), or 0 when `at` isn't such a call.
	DWORD call(DWORD at, const void *hook, const char *what);

	// `size` bytes at `at`, which must be exactly `expected`, become a call to `hook` followed by
	// NOPs (the hook re-runs the replaced instructions).
	bool callOver(DWORD at, const uint8_t *expected, size_t size, const void *hook, const char *what);

	// A function's entry, starting with the compiler's SEH prologue (push -1; push imm32) or with
	// another mod's jump, now jumps to `hook`. Returns a callable original (a trampoline), or null.
	void *entry(DWORD at, const void *hook, const char *what);

	// A pointer slot (a vtable entry, an import) now holds `value`. Returns the previous pointer, or
	// 0 (and leaves the slot alone) when it doesn't point at executable code.
	DWORD pointer(DWORD slot, const void *value, const char *what);

	template<typename F>
	F call(DWORD at, F hook, const char *what) { return reinterpret_cast<F>(call(at, reinterpret_cast<const void *>(hook), what)); }
	template<typename F>
	F pointer(DWORD slot, F value, const char *what) { return reinterpret_cast<F>(pointer(slot, reinterpret_cast<const void *>(value), what)); }
}
