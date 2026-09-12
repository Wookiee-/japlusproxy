// patch.c — mprotect + rel32-jmp trampoline for x86 32-bit.
// Build only for Linux i386 (-m32).

#define _GNU_SOURCE
#include "patch.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

void *ModuleBase(void *sym_inside) {
  Dl_info di;
  memset(&di, 0, sizeof(di));
  if (dladdr(sym_inside, &di) && di.dli_fbase) return di.dli_fbase;
  return NULL;
}

static int MakeWritable(void *addr, size_t len) {
  long ps = sysconf(_SC_PAGESIZE);
  if (ps <= 0) ps = 4096;
  uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(ps - 1);
  uintptr_t end = ((uintptr_t)addr + len + (uintptr_t)(ps - 1)) & ~(uintptr_t)(ps - 1);
  return mprotect((void*)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC);
}

int PatchBytes(void *dst, const void *src, size_t len) {
  if (!dst || !src || !len) return -1;
  if (MakeWritable(dst, len) != 0) return -1;
  memcpy(dst, src, len);
  __builtin___clear_cache((char*)dst, (char*)dst + len);
  return 0;
}

int HookJumpN(void *target, void *detour, size_t prefix_len,
              void **out_tramp) {
  unsigned char *t = (unsigned char *)target;
  unsigned char *tr;
  size_t total;
  if (!target || !detour || !out_tramp || prefix_len < 5 || prefix_len > 64)
    return -1;

  // trampoline: prefix bytes + 5-byte jmp back to target+prefix_len
  total = prefix_len + 5;
  tr = (unsigned char *)mmap(NULL, total,
      PROT_READ | PROT_WRITE | PROT_EXEC,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return -1;
  memcpy(tr, t, prefix_len);
  {
    intptr_t rel = (intptr_t)((t + prefix_len) - (tr + prefix_len + 5));
    int32_t r32 = (int32_t)rel;
    tr[prefix_len] = 0xE9;
    memcpy(tr + prefix_len + 1, &r32, 4);
  }

  // overwrite target with jmp rel32 -> detour, NOP-padded to prefix_len
  {
    unsigned char buf[64];
    intptr_t rel = (intptr_t)((unsigned char*)detour - (t + 5));
    int32_t r32 = (int32_t)rel;
    buf[0] = 0xE9;
    memcpy(buf + 1, &r32, 4);
    memset(buf + 5, 0x90, prefix_len - 5);
    if (PatchBytes(t, buf, prefix_len) != 0) { munmap(tr, total); return -1; }
  }
  *out_tramp = tr;
  return 0;
}

// ---------------------------------------------------------------------------
// JA+ target table — link-time vaddrs verified by disassembly
// (llvm-objdump, .symtab present in jampgamei386.so). Runtime address =
// module base (dladdr dli_fbase) + vaddr. `expect` interlocks every hook.
// Audit status (binary findings):
//  - NO strcpy/strcat/sprintf/system/gets imports anywhere in the lib;
//    only strncpy/sscanf/vsnprintf. strncpy sites checked NUL-terminate.
//  - All 19 sscanf sites mapped; the 2 client-reachable ones (ClientConnect
//    +0x74e, ClientBegin +0x700) use "%f" with return-value checks. Safe.
//  - Cmd_amvstr_f: admin gate (G_ClientAdminCmdAllowed) + testExecuteSeparator
//    + password-cvar blocklist (rconpassword, sv_privatePassword, jp_*Pass).
//  - Cmd_ammap_f: testExecuteSeparator x3 + admin gate. Consistent with the
//    vmMain-boundary filter in proxy.c (defense in depth, not a replacement).
//  - Cmd_SiegeClass_f validates via BG_SiegeCheckClassLegality, then applies
//    through ClientUserinfoChanged + ClientBegin.
// ---------------------------------------------------------------------------

typedef struct {
  const char *name;
  uintptr_t vaddr;          // JA+ link-time address
  unsigned char expect[6];  // required first bytes (interlock)
  size_t prefix_len;        // instruction-boundary-aligned hook length
} japlus_target_t;

static const japlus_target_t g_targets[] = {
  // Armed hook (see InstallPatches):
  { "BG_SiegeFindClassByName", 0x10DB34, {0x57,0x56,0x53,0x8B,0x15,0x00}, 9 },
  // Verified, reserved for future hooks (verify-only for now):
  { "ClientSpawn",             0x122DD4, {0x57,0x56,0x55,0x53,0x81,0xEC}, 0 },
  { "ClientCommand",           0x1B2E06, {0x57,0x56,0x55,0x53,0x81,0xEC}, 0 },
  { "ClientUserinfoChanged",   0x11F8FE, {0x57,0x56,0x55,0x53,0x81,0xEC}, 0 },
  { "SetTeam",                 0x19F338, {0x57,0x56,0x55,0x53,0x81,0xEC}, 0 },
  { "Cmd_SiegeClass_f",        0x1A041A, {0x57,0x56,0x55,0x53,0x83,0xEC}, 0 },
  { "G_ParseSpawnVars",        0x14E4A2, {0x55,0x81,0xEC,0x00,0x08,0x00}, 0 },
  { "Cmd_amvstr_f",            0x1AA4BC, {0x81,0xEC,0x00,0x08,0x00,0x00}, 0 },
  { "Cmd_amlogin_f",           0x1A470A, {0x55,0x81,0xEC,0x00,0x08,0x00}, 0 },
  { "Cmd_ammap_f",             0x1A5A56, {0x57,0x56,0x53,0x81,0xEC,0x00}, 0 },
};

static const japlus_target_t *FindTarget(const char *name) {
  size_t i;
  for (i = 0; i < sizeof(g_targets)/sizeof(g_targets[0]); i++)
    if (!strcmp(g_targets[i].name, name)) return &g_targets[i];
  return NULL;
}

static void *ResolveTarget(void *base, const japlus_target_t *t) {
  unsigned char *addr = (unsigned char *)base + t->vaddr;
  if (memcmp(addr, t->expect, sizeof(t->expect)) != 0) {
    fprintf(stderr, "[japlus_proxy] target %s@%p byte mismatch — "
      "wrong JA+ build, hook skipped\n", t->name, (void*)addr);
    return NULL;
  }
  return addr;
}

// BG_SiegeFindClassByName(const char *classname) -> siegeClass_t* / NULL.
// Matches OpenJK bg_saga.c (single arg, pointer return, NULL = not found).
// Detour rejects what can never legitimately match: NULL and overlong names
// (a NULL would crash Q_stricmp; 64+ chars exceed every real class token).
static void *g_tramp_siegeFind;
static void *Detour_SiegeFind(const char *classname) {
  void *(*orig)(const char *) = (void *(*)(const char *))g_tramp_siegeFind;
  if (!classname) return NULL;
  if (strlen(classname) >= 64) return NULL;
  return orig(classname);
}

void InstallPatches(void *real_handle) {
  void *vm = dlsym(real_handle, "vmMain");
  void *base = ModuleBase(vm ? vm : real_handle);
  size_t i;
  fprintf(stderr, "[japlus_proxy] real base=%p vmMain=%p\n", base, vm);
  if (!base) return;

  for (i = 0; i < sizeof(g_targets)/sizeof(g_targets[0]); i++) {
    const japlus_target_t *t = &g_targets[i];
    void *addr = ResolveTarget(base, t);
    fprintf(stderr, "[japlus_proxy] target %-22s %s\n",
      t->name, addr ? "verified" : "SKIPPED");
  }

  if (getenv("JAPLUS_NO_HOOKS")) {
    fprintf(stderr, "[japlus_proxy] inline hooks disabled by env\n");
    return;
  }

  {
    const japlus_target_t *t = FindTarget("BG_SiegeFindClassByName");
    void *addr = t ? ResolveTarget(base, t) : NULL;
    if (addr && HookJumpN(addr, (void*)Detour_SiegeFind,
                          t->prefix_len, &g_tramp_siegeFind) == 0)
      fprintf(stderr, "[japlus_proxy] hooked %s@%p tramp=%p\n",
        t->name, addr, g_tramp_siegeFind);
    else
      fprintf(stderr, "[japlus_proxy] siege hook not armed\n");
  }
}
