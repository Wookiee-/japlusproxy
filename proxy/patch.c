// patch.c — mprotect + rel32-jmp trampoline for x86 32-bit.
// Build only for Linux i386 (-m32).

#define _GNU_SOURCE
#include "patch.h"
#include "hde32.h"
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
// Planted-hook inventory: orig bytes saved before patching (q_engine-style
// RemoveHook support), plus read-back verification so startup logs prove
// each fix is really in — "hooked" is only printed after the planted jump
// is confirmed to point at our detour.
// ---------------------------------------------------------------------------
#define MAX_PLANTED 8
typedef struct {
  const char *name;
  void *target;
  unsigned char orig[8];
  size_t len;
} planted_t;
static planted_t g_planted[MAX_PLANTED];
static int g_planted_n = 0;

static void TrackHook(const char *name, void *target, size_t len) {
  if (g_planted_n >= MAX_PLANTED || !name || !target || !len || len > 8)
    return;
  g_planted[g_planted_n].name = name;
  g_planted[g_planted_n].target = target;
  memcpy(g_planted[g_planted_n].orig, target, len);
  g_planted[g_planted_n].len = len;
  g_planted_n++;
}

// Port of q_engine RemoveHook: restore every planted hook to orig bytes.
// Currently unused at runtime (game module lives until process exit);
// kept for future hot-reload support.
void RemoveHooks(void) {
  int i;
  for (i = 0; i < g_planted_n; i++)
    PatchBytes(g_planted[i].target, g_planted[i].orig, g_planted[i].len);
}

// Read back the planted jump and confirm it reaches detour.
static int HookArmed(const char *name, void *detour) {
  int i;
  for (i = 0; i < g_planted_n; i++) {
    unsigned char *t;
    if (strcmp(g_planted[i].name, name) != 0) continue;
    t = (unsigned char *)g_planted[i].target;
    if (t[0] == 0xE9) { // rel32 jmp (HookJumpN)
      int32_t r;
      memcpy(&r, t + 1, 4);
      return (void *)(t + 5 + r) == detour;
    }
    if (t[0] == 0x68 && t[5] == 0xC3) { // push+ret (HookJumpAbs)
      uint32_t a;
      memcpy(&a, t + 1, 4);
      return a == (uint32_t)(uintptr_t)detour;
    }
    return 0;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// YBE-style hook sizing + planting (HookUtils::GetLen/Attach), adapted:
// HookLenOk disassembles with hde32 (same vendored copy) and requires the
// recorded prefix_len to land exactly on an instruction boundary, so no
// hook can ever split an instruction. HookAuto prefers the 5-byte rel32
// jmp and falls back to absolute jumps only when the detour is out of
// rel32 range (engine .text sits ~3.8 GB from our .so — pure rel32, as
// YBE uses, mathematically cannot reach it). Byte interlocks, anchors,
// trampolines and read-back verification stay as ours.
// ---------------------------------------------------------------------------
static int HookLenOk(const void *addr, size_t want) {
  size_t n = 0;
  struct hde32s hs;
  if (!addr || !want || want > 64) return 0;
  while (n < want) {
    unsigned l = hde32_disasm((const unsigned char *)addr + n, &hs);
    if (!l || (hs.flags & F_ERROR)) return 0;
    n += l;
  }
  return n == want;
}

static int HookAuto(void *target, void *detour, size_t len, void **out_tramp) {
  intptr_t rel;
  if (!target || !detour || !out_tramp || !len || len > 64) return -1;
  rel = (unsigned char *)detour - ((unsigned char *)target + 5);
  if (len >= 5 && rel == (int32_t)rel)
    return HookJumpN(target, detour, len, out_tramp);
  if (len >= 6)
    return HookJumpAbs(target, detour, len, out_tramp);
  return -1;
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
  size_t expect_len;        // bytes of expect[] compared (<= 6)
  size_t prefix_len;        // instruction-boundary-aligned hook length
} japlus_target_t;
// NOTE: expect[] must never cover relocation bytes (loader-patched
// absolute addresses differ file-vs-runtime). BG_SiegeFindClassByName has
// R_386_32 at +5 (bgNumSiegeClasses), so only its first 5 bytes interlock;
// every other entry was checked relocation-free in its 6-byte window.

static const japlus_target_t g_targets[] = {
  // Armed hook (see InstallPatches):
  { "BG_SiegeFindClassByName", 0x10DB34, {0x57,0x56,0x53,0x8B,0x15,0x00}, 5, 9 },
  // Verified, reserved for future hooks (verify-only for now):
  { "ClientSpawn",             0x122DD4, {0x57,0x56,0x55,0x53,0x81,0xEC}, 6, 0 },
  { "ClientCommand",           0x1B2E06, {0x57,0x56,0x55,0x53,0x81,0xEC}, 6, 0 },
  { "ClientUserinfoChanged",   0x11F8FE, {0x57,0x56,0x55,0x53,0x81,0xEC}, 6, 0 },
  { "SetTeam",                 0x19F338, {0x57,0x56,0x55,0x53,0x81,0xEC}, 6, 0 },
  { "Cmd_SiegeClass_f",        0x1A041A, {0x57,0x56,0x55,0x53,0x83,0xEC}, 6, 0 },
  { "G_ParseSpawnVars",        0x14E4A2, {0x55,0x81,0xEC,0x00,0x08,0x00}, 6, 0 },
  { "Cmd_amvstr_f",            0x1AA4BC, {0x81,0xEC,0x00,0x08,0x00,0x00}, 6, 0 },
  { "Cmd_amlogin_f",           0x1A470A, {0x55,0x81,0xEC,0x00,0x08,0x00}, 6, 0 },
  { "Cmd_ammap_f",             0x1A5A56, {0x57,0x56,0x53,0x81,0xEC,0x00}, 6, 0 },
};

static const japlus_target_t *FindTarget(const char *name) {
  size_t i;
  for (i = 0; i < sizeof(g_targets)/sizeof(g_targets[0]); i++)
    if (!strcmp(g_targets[i].name, name)) return &g_targets[i];
  return NULL;
}

static void *ResolveTarget(void *base, const japlus_target_t *t) {
  unsigned char *addr = (unsigned char *)base + t->vaddr;
  size_t n = t->expect_len <= sizeof(t->expect) ? t->expect_len : sizeof(t->expect);
  if (n == 0 || memcmp(addr, t->expect, n) != 0) {
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
// SIGNATURE (verified against the sole caller SV_PacketEvent@0x0805718A,
// which pushes a 20-byte struct copy + msg = 6 dwords): the address comes
// BY VALUE — void (*)(netadr_t_20B from, msg_t *msg). Treating arg0 as a
// pointer faults (arg0 is type=NA_IP=4, so from+4 touches 0x8). The detour
// below mirrors the by-value layout; gdb frames past the caller then show
// struct words, not return addresses — that is normal here.
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

static oob_bucket_t *BucketFor(oob_bucket_t *t, int n, uint32_t ipi) {
  int i, victim = -1;
  for (i = 0; i < n; i++) {
    if (t[i].used) {
      if (t[i].ipi == ipi) return &t[i];
      if (victim < 0 || t[i].lastTime < t[victim].lastTime)
        victim = i;
    } else {
      victim = i; // free slot beats evicting the oldest entry
      break;
    }
  }
  if (victim < 0) victim = 0;
  memset(&t[victim], 0, sizeof(oob_bucket_t));
  t[victim].ipi = ipi;
  t[victim].used = 1;
  return &t[victim];
}

static oob_bucket_t *OOB_Bucket(uint32_t ipi) {
  return BucketFor(oob_table, OOB_BUCKETS, ipi);
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

// Stock 20-byte netadr_t as passed by value (only type+ip are read).
typedef struct { uint32_t w[5]; } oob_from_t;

static void *g_tramp_connless;
static void Detour_Connless(oob_from_t from, const void *msg) {
  void (*orig)(oob_from_t, const void *) =
    (void (*)(oob_from_t, const void *))g_tramp_connless;
  int now;
  if (OOB_ShouldDrop(&from)) {
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

static void *ResolveEngineAbs(const char *name, uintptr_t off,
                              const unsigned char *expect, size_t elen,
                              uint32_t anchor, unsigned scan) {
  unsigned char *addr = (unsigned char *)(uintptr_t)(ENGINE_BASE + off);
  unsigned i;
  if (!expect || !elen || elen > 8 ||
      memcmp(addr, expect, elen) != 0) {
    fprintf(stderr, "[japlus_proxy] engine %s@%p "
      "byte mismatch — wrong engine build, hook skipped\n", name, (void*)addr);
    return NULL;
  }
  for (i = 0; i + 4 <= scan; i++) {
    uint32_t w;
    memcpy(&w, addr + i, 4);
    if (w == anchor) break;
  }
  if (i + 4 > scan) {
    fprintf(stderr, "[japlus_proxy] engine %s@%p "
      "anchor not found — hook skipped\n", name, (void*)addr);
    return NULL;
  }
  return addr;
}

static void *ResolveEngineConnless(void) {
  static const unsigned char expect[6] =
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20};
  return ResolveEngineAbs("SV_ConnectionlessPacket", ENGINE_CONNLESS_OFF,
                          expect, sizeof(expect),
                          ENGINE_CONNLESS_STR, ENGINE_CONNLESS_SCAN);
}

// ---------------------------------------------------------------------------
// SV_DoneDownload_f (engine "donedl" handler) — respawn-cheat guard.
// Clients spam donedl to force a gamestate/respawn outside game rules.
// The command is engine-consumed (dispatch table, never crosses vmMain),
// so only an engine hook can touch it. Detour drops it while downloads
// are disabled (sv_allowdownload == 0, our default); with downloads on,
// legit clients need the handshake, so it passes through.
// Target 0x0804EAA4: prologue 55 8B EC 83 EC 10 (prefix 6, boundary at
// +6), anchor = clientDownload string DWORD 0x0819B5B8.
// ---------------------------------------------------------------------------

#define ENGINE_DONEDL_OFF 0x6AA4u
#define ENGINE_DONEDL_PREFIX 6u
#define ENGINE_DONEDL_STR 0x0819B5B8u
#define ENGINE_DONEDL_SCAN 0x100u

static void *g_tramp_donedl;
static int oob_dl_dropped, oob_dl_lastMsg;
static void Detour_DoneDownload(const void *cl) {
  void (*orig)(const void *) =
    (void (*)(const void *))g_tramp_donedl;
  int now;
  if (Proxy_CvarInt("sv_allowdownload") != 0) {
    orig(cl);
    return;
  }
  oob_dl_dropped++;
  now = OOB_NowMs();
  if (oob_dl_lastMsg + 5000 < now) {
    fprintf(stderr, "[japlus_proxy] donedl dropped %d (downloads disabled)\n",
      oob_dl_dropped);
    oob_dl_lastMsg = now;
    oob_dl_dropped = 0;
  }
}

static void *ResolveEngineDonedl(void) {
  static const unsigned char expect[6] =
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10};
  return ResolveEngineAbs("SV_DoneDownload_f", ENGINE_DONEDL_OFF,
                          expect, sizeof(expect),
                          ENGINE_DONEDL_STR, ENGINE_DONEDL_SCAN);
}

// ---------------------------------------------------------------------------
// SVC_RemoteCommand (engine "rcon" handler) — brute-force throttle.
// Stock has no per-IP rcon limit; password guessing is otherwise free.
// Detour allows a short burst (legit admin typing) then 1 attempt per 2 s
// per IP; excess attempts are dropped before the password is even tested.
// Same by-value (netadr_t, msg_t*) family as SV_ConnectionlessPacket.
// Target 0x08056B14: prologue 55 8B EC 81 EC 20 C5 00 00 (prefix 9),
// anchor = "Bad rconpassword." DWORD 0x0819D5B0.
// ---------------------------------------------------------------------------

#define ENGINE_RCON_OFF 0xEB14u
#define ENGINE_RCON_PREFIX 9u
#define ENGINE_RCON_STR 0x0819D5B0u
#define ENGINE_RCON_SCAN 0x400u
#define RCON_BUCKETS 64
#define RCON_BURST 5
#define RCON_PERIOD 2000

static oob_bucket_t rcon_table[RCON_BUCKETS];
static int rcon_dropped, rcon_lastMsg;
static void *g_tramp_rcon;
static void Detour_Rcon(oob_from_t from, const void *msg) {
  void (*orig)(oob_from_t, const void *) =
    (void (*)(oob_from_t, const void *))g_tramp_rcon;
  uint32_t ipi;
  oob_bucket_t *b;
  int now;
  memcpy(&ipi, &from.w[1], 4);
  if (ipi != 0x0100007Fu) { // loopback exempt (local admin tools)
    now = OOB_NowMs();
    b = BucketFor(rcon_table, RCON_BUCKETS, ipi);
    if (OOB_RateLimit(&b->burst, &b->lastTime, RCON_BURST, RCON_PERIOD, now)) {
      rcon_dropped++;
      if (rcon_lastMsg + 5000 < now) {
        fprintf(stderr, "[japlus_proxy] rcon throttled %d attempts\n",
          rcon_dropped);
        rcon_lastMsg = now;
        rcon_dropped = 0;
      }
      return;
    }
  }
  orig(from, msg);
}

static void *ResolveEngineRcon(void) {
  static const unsigned char expect[6] =
    {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x20};
  return ResolveEngineAbs("SVC_RemoteCommand", ENGINE_RCON_OFF,
                          expect, sizeof(expect),
                          ENGINE_RCON_STR, ENGINE_RCON_SCAN);
}

static int g_hook_try, g_hook_ok;

// Full plant sequence for one target: verify line, length check, track,
// plant, read-back. Standard log lines; ends with the protections summary.
static void PlantHook(const char *name, void *addr, size_t prefix_len,
                      void *detour, void **tramp) {
  fprintf(stderr, "[japlus_proxy] target %-22s %s\n",
    name, addr ? "verified" : "SKIPPED");
  if (!addr) return;
  g_hook_try++;
  if (!HookLenOk(addr, prefix_len)) {
    fprintf(stderr, "[japlus_proxy] target %s@%p length mismatch — hook skipped\n",
      name, addr);
    return;
  }
  TrackHook(name, addr, prefix_len);
  if (HookAuto(addr, detour, prefix_len, tramp) == 0 &&
      HookArmed(name, detour)) {
    g_hook_ok++;
    fprintf(stderr, "[japlus_proxy] hooked %s@%p tramp=%p\n",
      name, addr, *tramp);
  } else {
    fprintf(stderr, "[japlus_proxy] %s hook not armed\n", name);
  }
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
    PlantHook(t ? t->name : "BG_SiegeFindClassByName", addr,
              t ? t->prefix_len : 0,
              (void*)Detour_SiegeFind, &g_tramp_siegeFind);
  }

  if (getenv("JAPLUS_NO_OOB")) {
    fprintf(stderr, "[japlus_proxy] OOB hook disabled by env\n");
  } else {
    PlantHook("SV_ConnectionlessPacket", ResolveEngineConnless(),
              ENGINE_CONNLESS_PREFIX,
              (void*)Detour_Connless, &g_tramp_connless);
  }

  PlantHook("SV_DoneDownload_f", ResolveEngineDonedl(),
            ENGINE_DONEDL_PREFIX,
            (void*)Detour_DoneDownload, &g_tramp_donedl);

  PlantHook("SVC_RemoteCommand", ResolveEngineRcon(),
            ENGINE_RCON_PREFIX,
            (void*)Detour_Rcon, &g_tramp_rcon);

  fprintf(stderr, "[japlus_proxy] protections: filters=on hooks_armed=%d/%d\n",
    g_hook_ok, g_hook_try);
}
