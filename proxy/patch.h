#pragma once
// Inline-patch / trampoline helpers (Linux x86 32-bit) for the closed JA+
// lib. Targets are JA+ link-time vaddrs (see table in patch.c, verified by
// disassembly); runtime address = module base (dladdr) + vaddr.
// Every hook is byte-interlocked: first bytes must match the recorded
// prologue or the hook is skipped with a log line — a wrong-build .so can
// never be patched blindly.
//
// prefix_len MUST end on an instruction boundary >= 5 (verified per target
// with llvm-objdump; blindly using 5 can split an instruction and crash).

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called once after dlopen, with the real lib handle.
void InstallPatches(void *real_handle);
// Restore every planted hook to its saved orig bytes (q_engine RemoveHook
// port; for future hot-reload — the game module normally lives until exit).
void RemoveHooks(void);

// Low-level helpers (x86 32-bit, rel32 jmp):
// Overwrite `len` bytes at `dst` with `src` (mprotect-safe). Returns 0 ok.
int PatchBytes(void *dst, const void *src, size_t len);
// Hook `target` -> `detour`, preserving `prefix_len` leading bytes in an
// executable trampoline (*out_tramp) that resumes at target+prefix_len.
// The 5-byte jmp is NOP-padded to prefix_len. Returns 0 ok.
int HookJumpN(void *target, void *detour, size_t prefix_len,
              void **out_tramp);
// Absolute-jump hook (push+ret / mov+jmp, no rel32 range limit) for engine
// targets our .so can't reach with rel32. prefix_len must be >= 6 and end
// on an instruction boundary. Returns 0 ok.
int HookJumpAbs(void *target, void *detour, size_t prefix_len,
                void **out_tramp);
// Base address (dli_fbase) of the module containing `sym_inside`.
void *ModuleBase(void *sym_inside);
// Engine cvar read from proxy.c (0 when the engine isn't up / cvar absent).
int Proxy_CvarInt(const char *name);

#ifdef __cplusplus
}
#endif
