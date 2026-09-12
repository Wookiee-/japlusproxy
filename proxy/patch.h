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

// Low-level helpers (x86 32-bit, rel32 jmp):
// Overwrite `len` bytes at `dst` with `src` (mprotect-safe). Returns 0 ok.
int PatchBytes(void *dst, const void *src, size_t len);
// Hook `target` -> `detour`, preserving `prefix_len` leading bytes in an
// executable trampoline (*out_tramp) that resumes at target+prefix_len.
// The 5-byte jmp is NOP-padded to prefix_len. Returns 0 ok.
int HookJumpN(void *target, void *detour, size_t prefix_len,
              void **out_tramp);
// Base address (dli_fbase) of the module containing `sym_inside`.
void *ModuleBase(void *sym_inside);

#ifdef __cplusplus
}
#endif
