// patch.c — mprotect + rel32-jmp trampoline for x86 32-bit.
// Build only for Linux i386 (-m32).

#define _GNU_SOURCE
#include "patch.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

// Absolute-jump hook for targets a rel32 can't reach: the engine .text
// lives at fixed low addresses (non-PIE, ~0x08000000) while our .so is
// mapped ~3.8 GB away, overflowing int32. Both jumps are absolute:
//   target: push detour_imm32; ret            (6 bytes, NOP-padded past 6)
//   tramp : prefix + mov eax,imm32; jmp eax   (7 bytes)
// push+ret is stack-neutral and touches no flags.
int HookJumpAbs(void *target, void *detour, size_t prefix_len,
                void **out_tramp) {
  unsigned char *t = (unsigned char *)target;
  unsigned char *tr;
  size_t total;
  uint32_t addr32;
  if (!target || !detour || !out_tramp || prefix_len < 6 || prefix_len > 64)
    return -1;

  total = prefix_len + 7;
  tr = (unsigned char *)mmap(NULL, total,
      PROT_READ | PROT_WRITE | PROT_EXEC,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tr == MAP_FAILED) return -1;
  memcpy(tr, t, prefix_len);
  addr32 = (uint32_t)(uintptr_t)(t + prefix_len);
  tr[prefix_len] = 0xB8;
  memcpy(tr + prefix_len + 1, &addr32, 4);
  tr[prefix_len + 5] = 0xFF;
  tr[prefix_len + 6] = 0xE0;

  {
    unsigned char buf[64];
    addr32 = (uint32_t)(uintptr_t)detour;
    buf[0] = 0x68;
    memcpy(buf + 1, &addr32, 4);
    buf[5] = 0xC3;
    memset(buf + 6, 0x90, prefix_len - 6);
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

// ---------------------------------------------------------------------------
// Engine OOB (connectionless-packet) flood guard — port of OpenJK's
// SVC_RateLimit + per-address buckets (codemp/server/sv_main.cpp).
//
// Why here: stock linuxjampded has NO sv_maxOOBRate* throttling (verified:
// those cvars/strings are absent), so spoofed getstatus/getinfo/
// getchallenge/connect/rcon floods each cost a full handler run. The game
// module never sees these packets, so proxy.c filters can't help — the
// bucket check runs inside the engine's own SV_ConnectionlessPacket.
//
// Target: SV_ConnectionlessPacket(from, msg). The engine is a non-PIE EXEC
// (fixed base 0x08048000), found via the "getstatus" string xref and
// verified with capstone against the local binary:
//   prologue 55 8B EC 83 EC 20 (push ebp; mov ebp,esp; sub esp,0x20),
//   prefix 6 ends exactly on a boundary (next insn: 89 7D FC).
// Interlock: prologue bytes must match AND the getstatus-string DWORD must
// appear within the first 0x400 bytes (proves this is the dispatch fn).
// Rel32 can't reach from engine .text to our .so, so this hook uses
// HookJumpAbs (absolute jumps). Engine packet handling is single-threaded,
// so the bucket table needs no locks.
// ---------------------------------------------------------------------------

#define ENGINE_BASE 0x08048000u
#define ENGINE_CONNLESS_OFF 0xED64u    // .text fileoff == vaddr - base
#define ENGINE_CONNLESS_PREFIX 6u
#define ENGINE_CONNLESS_STR 0x0819D654u // "getstatus" in .data (anchor)
#define ENGINE_CONNLESS_SCAN 0x400u

static int OOB_NowMs(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// Exact port of OpenJK SVC_RateLimit. Returns 1 = over limit, drop.
static int OOB_RateLimit(int *burst, int *lastTime, int maxBurst,
                         int period, int now) {
  int interval, expired, expiredRemainder;
  if (!burst || !lastTime || period <= 0 || maxBurst <= 0) return 0;
  interval = now - *lastTime;
  expired = interval / period;
  expiredRemainder = interval % period;
  if (expired > *burst || interval < 0) {
    *burst = 0;
    *lastTime = now;
  } else {
    *burst -= expired;
    *lastTime = now - expiredRemainder;
  }
  if (*burst < maxBurst) {
    (*burst)++;
    return 0;
  }
  return 1;
}

#define OOB_BUCKETS 256
typedef struct { uint32_t ipi; int lastTime; int burst; int used; } oob_bucket_t;
static oob_bucket_t oob_table[OOB_BUCKETS];
static int oob_global_burst, oob_global_last;
static int oob_dropped, oob_lastMsg;
static int oob_cfg_ip = -1, oob_cfg_all = -1, oob_cfg_time = 0;

static void OOB_RefreshCfg(int now) {
  int v;
  if (oob_cfg_time && now - oob_cfg_time < 5000) return; // bound syscall rate
  oob_cfg_time = now;
  // Stock engine lacks these cvars (read -> 0): 0 means "absent", so fall
  // back to OpenJK's defaults with protection ON. Positive values override.
  v = Proxy_CvarInt("sv_maxOOBRateIP");
  oob_cfg_ip = (v > 0) ? (v > 1000 ? 1000 : v) : 1;
  v = Proxy_CvarInt("sv_maxOOBRate");
  oob_cfg_all = (v > 0) ? (v > 1000 ? 1000 : v) : 1000;
}

static oob_bucket_t *OOB_Bucket(uint32_t ipi) {
  int i, victim = -1;
  for (i = 0; i < OOB_BUCKETS; i++) {
    if (oob_table[i].used) {
      if (oob_table[i].ipi == ipi) return &oob_table[i];
      if (victim < 0 || oob_table[i].lastTime < oob_table[victim].lastTime)
        victim = i;
    } else {
      victim = i; // free slot beats evicting the oldest entry
      break;
    }
  }
  if (victim < 0) victim = 0;
  memset(&oob_table[victim], 0, sizeof(oob_bucket_t));
  oob_table[victim].ipi = ipi;
  oob_table[victim].used = 1;
  return &oob_table[victim];
}

// Minimal netadr mirror: only type(+0) and ip(+4) are read, so trailing
// engine fields (port, sock, ...) can differ without breaking us.
static int OOB_ShouldDrop(const void *from) {
  uint32_t ipi;
  int now, period;
  oob_bucket_t *b;
  memcpy(&ipi, (const unsigned char *)from + 4, 4);
  if (ipi == 0x0100007Fu) return 0; // loopback exempt (local rcon/tools)
  now = OOB_NowMs();
  OOB_RefreshCfg(now);
  period = 1000 / oob_cfg_ip; // per-IP: burst 10x rate, like OpenJK
  b = OOB_Bucket(ipi);
  if (OOB_RateLimit(&b->burst, &b->lastTime, 10 * oob_cfg_ip, period, now))
    return 1;
  period = 1000 / oob_cfg_all; // global aggregate backstop (spoofed floods)
  if (OOB_RateLimit(&oob_global_burst, &oob_global_last,
                    oob_cfg_all, period, now))
    return 1;
  return 0;
}

static void *g_tramp_connless;
static void Detour_Connless(const void *from, const void *msg) {
  void (*orig)(const void *, const void *) =
    (void (*)(const void *, const void *))g_tramp_connless;
  int now;
  if (from && OOB_ShouldDrop(from)) {
    oob_dropped++;
    now = OOB_NowMs();
    if (oob_lastMsg + 5000 < now) {
      fprintf(stderr, "[japlus_proxy] OOB rate limit: dropped %d "
        "connectionless requests\n", oob_dropped);
      oob_lastMsg = now;
      oob_dropped = 0;
    }
    return;
  }
  orig(from, msg);
}

static void *ResolveEngineConnless(void) {
  unsigned char *addr =
    (unsigned char *)(uintptr_t)(ENGINE_BASE + ENGINE_CONNLESS_OFF);
  static const unsigned char expect[6] =
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20};
  unsigned i;
  if (memcmp(addr, expect, sizeof(expect)) != 0) {
    fprintf(stderr, "[japlus_proxy] engine SV_ConnectionlessPacket@%p "
      "byte mismatch — wrong engine build, hook skipped\n", (void*)addr);
    return NULL;
  }
  for (i = 0; i + 4 <= ENGINE_CONNLESS_SCAN; i++) {
    uint32_t w;
    memcpy(&w, addr + i, 4);
    if (w == ENGINE_CONNLESS_STR) break;
  }
  if (i + 4 > ENGINE_CONNLESS_SCAN) {
    fprintf(stderr, "[japlus_proxy] engine SV_ConnectionlessPacket@%p "
      "anchor not found — hook skipped\n", (void*)addr);
    return NULL;
  }
  return addr;
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

  {
    void *addr = ResolveEngineConnless();
    fprintf(stderr, "[japlus_proxy] target %-22s %s\n",
      "SV_ConnectionlessPacket", addr ? "verified" : "SKIPPED");
    if (addr && HookJumpAbs(addr, (void*)Detour_Connless,
                            ENGINE_CONNLESS_PREFIX, &g_tramp_connless) == 0)
      fprintf(stderr, "[japlus_proxy] hooked SV_ConnectionlessPacket@%p tramp=%p\n",
        addr, g_tramp_connless);
    else if (addr)
      fprintf(stderr, "[japlus_proxy] OOB hook not armed\n");
  }
}
