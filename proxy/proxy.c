// japlus_proxy — wrapper .so trampoline/middleman for legacy JA+ jampgamei386.so.
//
// Target ABI is LEGACY Quake3 (NOT OpenJK GetModuleAPI):
//   void dllEntry( syscall_t engine_syscall );
//   intptr_t vmMain( int cmd, intptr_t a0..a11 );
// Verified against your binary: exports vmMain + dllEntry, no GetModuleAPI.
// OpenJK reference for IDs/logic: OpenJK/codemp/game/g_public.h (gameImportLegacy_e,
// gameExportLegacy_e), g_main.c vmMain(), g_syscalls.c dllEntry().
//
// Deploy: build produces japlusproxy.so. Place it where the engine loads the
// game module from, under the name the engine expects:
//   stock jampded (loads "jampgamei386.so"): copy japlusproxy.so over that name
//     and keep the original JA+ lib as ./japlus_real_i386.so (or set JAPLUS_REAL).
//   custom engine loading "japlusproxy.so": drop it next to the untouched
//     jampgamei386.so; the proxy skips itself when resolving the real lib.
// Engine loads us, we dlopen the real lib, forward + filter both directions:
//   engine -> game : via our vmMain  (filter CLIENT_COMMAND / CONNECT / BEGIN /
//             USERINFO incl. spawn-adjacent siegeclass/saber keys)
//   game   -> engine: via our syscall (filter SEND_SERVER_COMMAND / CONFIGSTRING /
//             USERCMD / ENTITY_TOKEN / ...)
// Plus optional inline hooks into the real lib, see patch.h / InstallPatches().
//
// Build (Linux i386): see Makefile. Requires gcc -m32.

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <stdlib.h>
#include "patch.h"

#ifndef QDECL
#define QDECL
#endif

// ---- legacy syscall IDs (OpenJK/codemp/game/g_public.h: gameImportLegacy_e) ----
enum {
  G_PRINT = 0,
  G_ERROR = 1,
  G_ARGC = 10,
  G_ARGV = 11,
  G_SEND_CONSOLE_COMMAND = 16,
  G_DROP_CLIENT = 18,
  G_SEND_SERVER_COMMAND = 19,
  G_SET_CONFIGSTRING = 20,
  G_GET_CONFIGSTRING = 21,
  G_GET_USERINFO = 22,
  G_SET_USERINFO = 23,
  G_CVAR_VARIABLE_INTEGER_VALUE = 8,
  G_GET_USERCMD = 40,
  G_GET_ENTITY_TOKEN = 41,
};

// ---- legacy game commands (g_public.h: gameExportLegacy_e) ----
enum {
  GAME_INIT = 0,
  GAME_SHUTDOWN = 1,
  GAME_CLIENT_CONNECT = 2,
  GAME_CLIENT_BEGIN = 3,
  GAME_CLIENT_USERINFO_CHANGED = 4,
  GAME_CLIENT_DISCONNECT = 5,
  GAME_CLIENT_COMMAND = 6,
  GAME_CLIENT_THINK = 7,
  GAME_RUN_FRAME = 8,
  GAME_CONSOLE_COMMAND = 9,
};

typedef intptr_t (QDECL *syscall_t)(intptr_t cmd, ...);
typedef void (QDECL *dllEntry_t)(syscall_t engine);
typedef intptr_t (QDECL *vmMain_t)(int cmd,
  intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4,
  intptr_t a5, intptr_t a6, intptr_t a7, intptr_t a8, intptr_t a9,
  intptr_t a10, intptr_t a11);

// Forward: our own exported entry point (defined below). TryAdoptReal
// compares candidate vmMain pointers against it to avoid adopting ourselves.
intptr_t QDECL vmMain(int cmd,
  intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4,
  intptr_t a5, intptr_t a6, intptr_t a7, intptr_t a8, intptr_t a9,
  intptr_t a10, intptr_t a11);

#ifndef REAL_LIB
#define REAL_LIB "./japlus_real_i386.so"
#endif
// Fallback when REAL_LIB is missing: the stock JA+ filename. LoadReal skips
// any candidate that resolves to this module itself (deploy-as-jampgame case).
#ifndef REAL_LIB_FALLBACK
#define REAL_LIB_FALLBACK "./jampgamei386.so"
#endif

static syscall_t  g_engine = NULL;
static void      *g_real   = NULL;
static dllEntry_t real_dllEntry = NULL;
static vmMain_t   real_vmMain   = NULL;
static int        g_patches_done = 0;

static int TryAdoptReal(const char *path) {
  void *h;
  vmMain_t vm;
  dllEntry_t de;
  if (!path || !*path) return 0;
  h = dlopen(path, RTLD_NOW | RTLD_DEEPBIND);
  if (!h) return 0;
  vm = (vmMain_t)dlsym(h, "vmMain");
  if (vm == (vmMain_t)&vmMain) { dlclose(h); return 0; } // that's us, not JA+
  de = (dllEntry_t)dlsym(h, "dllEntry");
  if (!vm || !de) { dlclose(h); return 0; }
  g_real = h;
  real_vmMain = vm;
  real_dllEntry = de;
  return 1;
}

// The real lib always sits next to the wrapper in both deploy layouts,
// but CWD-relative fallbacks break when the engine's working directory
// isn't the lib dir (e.g. wrapper in japlus/, CWD in GameData root).
// Resolve candidates relative to our own file location first.
static void TryAdoptRealNextToSelf(void) {
  Dl_info di;
  const char *self, *slash;
  char dir[1024], cand[1152];
  memset(&di, 0, sizeof(di));
  if (!dladdr((void *)&vmMain, &di) || !di.dli_fname) return;
  self = di.dli_fname;
  slash = strrchr(self, '/');
  if (!slash) return; // bare filename: CWD fallbacks below already cover it
  if ((size_t)(slash - self) >= sizeof(dir)) return;
  memcpy(dir, self, (size_t)(slash - self));
  dir[slash - self] = '\0';
  snprintf(cand, sizeof(cand), "%s/%s", dir, "japlus_real_i386.so");
  if (TryAdoptReal(cand)) return;
  snprintf(cand, sizeof(cand), "%s/%s", dir, "jampgamei386.so");
  TryAdoptReal(cand);
}

static void LoadReal(void) {
  const char *env;
  if (g_real) return;
  env = getenv("JAPLUS_REAL");
  if (env && *env) TryAdoptReal(env);
  if (!g_real) TryAdoptRealNextToSelf();
  if (!g_real) TryAdoptReal(REAL_LIB);
  if (!g_real) TryAdoptReal(REAL_LIB_FALLBACK);
  if (!g_real) {
    fprintf(stderr, "[japlus_proxy] could not load real game lib "
      "(tried JAPLUS_REAL, wrapper dir, %s, %s): %s\n",
      REAL_LIB, REAL_LIB_FALLBACK, dlerror());
    return;
  }
  if (!g_patches_done) {
    g_patches_done = 1;
    InstallPatches(g_real); // patch.c — offset hooks into real lib
  }
}

// ---- helpers that talk back to the engine ----
static int Engine_Argc(void) {
  return (int)g_engine((intptr_t)G_ARGC, 0,0,0,0,0,0,0,0,0,0,0,0);
}
static void Engine_Argv(int n, char *buf, int len) {
  if (len <= 0) return;
  buf[0] = '\0';
  g_engine((intptr_t)G_ARGV, (intptr_t)n, (intptr_t)buf, (intptr_t)len,
    0,0,0,0,0,0,0,0,0);
}
static void Engine_GetUserinfo(int client, char *buf, int len) {
  if (len <= 0) return;
  buf[0] = '\0';
  g_engine((intptr_t)G_GET_USERINFO, (intptr_t)client, (intptr_t)buf,
    (intptr_t)len, 0,0,0,0,0,0,0,0);
}
static void Engine_SetUserinfo(int client, const char *buf) {
  g_engine((intptr_t)G_SET_USERINFO, (intptr_t)client, (intptr_t)buf,
    0,0,0,0,0,0,0,0,0);
}
static int Engine_CvarInt(const char *name) {
  return (int)g_engine((intptr_t)G_CVAR_VARIABLE_INTEGER_VALUE,
    (intptr_t)name, 0,0,0,0,0,0,0,0,0,0,0);
}
// Engine cvar read for patch.c (OOB rate-limit tuning). Safe before the
// engine is up: returns 0 and callers fall back to compiled defaults.
int Proxy_CvarInt(const char *name) {
  if (!g_engine || !name) return 0;
  return Engine_CvarInt(name);
}

// ---- usercmd_t mirror (OpenJK codemp/qcommon/q_shared.h: usercmd_s) ----
// Engine<->game net struct; layout is protocol-frozen, JA+ matches stock.
// Only the fields we sanitize are named; offsets asserted at compile time.
typedef struct {
  int serverTime;   // +0
  int angles[3];    // +4  (PITCH 0, YAW 1, ROLL 2)
  int buttons;      // +16
  unsigned char weapon;        // +20
  unsigned char forcesel;      // +21
  unsigned char invensel;      // +22
  unsigned char generic_cmd;   // +23
  signed char forwardmove, rightmove, upmove; // +24,25,26
} proxy_usercmd_t;
#define PROXY_FP_LEVITATION 1     // forcePowers_t FP_LEVITATION (q_shared.h)
#define PROXY_NUM_FORCE_POWERS 19 // forcePowers_t NUM_FORCE_POWERS
#define PROXY_ROLL 2
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(proxy_usercmd_t, forcesel) == 21,
  "usercmd_t.forcesel offset");
_Static_assert(offsetof(proxy_usercmd_t, angles) == 4,
  "usercmd_t.angles offset");
_Static_assert(sizeof(proxy_usercmd_t) <= 32, "usercmd_t size");
#endif

// ---- game -> engine direction: syscall filter ----
// NOTE: fixed-arg trampoline is intentional: cdecl lets us forward a
// bounded arg list to the varargs engine dispatcher.
intptr_t QDECL Proxy_Syscall(intptr_t cmd,
  intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4, intptr_t a5, intptr_t a6,
  intptr_t a7, intptr_t a8, intptr_t a9, intptr_t a10, intptr_t a11, intptr_t a12)
{
  if (!g_engine) return 0;

  if (cmd == G_GET_USERCMD) {
    // Game pulls the latest client input here (covers GAME_CLIENT_THINK).
    // Sanitize AFTER the engine fills it: OOB forcesel indexes server-side
    // force arrays (crash/info-leak), FP_LEVITATION must never be selectable
    // (jump-hold physics abuse), nonzero ROLL is packet abuse (aim/physics).
    intptr_t ret = g_engine(cmd, a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12);
    proxy_usercmd_t *ucmd = (proxy_usercmd_t *)a2;
    if (ucmd) {
      if (ucmd->forcesel == PROXY_FP_LEVITATION ||
          ucmd->forcesel >= PROXY_NUM_FORCE_POWERS)
        ucmd->forcesel = 0xFFu;
      ucmd->angles[PROXY_ROLL] = 0;
    }
    return ret;
  }
  if (cmd == G_GET_ENTITY_TOKEN) {
    // Spawn-var parsing: the game drains the map entity string through here
    // at level load (G_ParseSpawnVars). Entity data ships with the map, so a
    // hostile/malformed .bsp is the vector, not a remote client — still worth
    // hardening since overlong tokens flow into fixed game-side buffers.
    // Policy is deliberately non-mutating for legit maps: NUL-guarantee +
    // MAX_TOKEN_CHARS cap with a log line; overlong tokens are truncated.
    intptr_t ret = g_engine(cmd, a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12);
    if (ret) {
      char *buf = (char *)a1;
      int size = (int)a2;
      if (buf && size > 0) {
        size_t cap;
        buf[size - 1] = '\0';
        cap = (size - 1 < 1023) ? (size_t)(size - 1) : 1023u;
        if (strlen(buf) > cap) {
          buf[cap] = '\0';
          g_engine((intptr_t)G_PRINT,
            (intptr_t)"[japlus_proxy] truncated overlong entity token\n",
            0,0,0,0,0,0,0,0,0,0);
        }
      }
    }
    return ret;
  }
  if (cmd == G_SEND_SERVER_COMMAND) {
    const char *s = (const char *)a2;
    if (s && strlen(s) > 1022) {
      // mirrors OpenJK g_syscalls.c trap_SendServerCommand guard — classic overflow/crash vector
      g_engine((intptr_t)G_PRINT, (intptr_t)"[japlus_proxy] blocked oversize servercommand\n",
        0,0,0,0,0,0,0,0,0,0,0);
      return 0;
    }
  }
  if (cmd == G_SET_CONFIGSTRING) {
    // CS range for JKA MP is small (see OpenJK bg_public.h CS_*); hard-drop garbage index.
    // MAX_CONFIGSTRINGS ~ 1700ish depending on build; keep conservative.
    if (a1 < 0 || a1 > 2048) {
      g_engine((intptr_t)G_PRINT, (intptr_t)"[japlus_proxy] blocked bad configstring index\n",
        0,0,0,0,0,0,0,0,0,0,0);
      return 0;
    }
    const char *s = (const char *)a2;
    if (s && strlen(s) >= 8192) return 0;
  }
  if (cmd == G_DROP_CLIENT) {
    // pass through, but log — useful exploit audit trail
    // (keep quiet to avoid log spam; uncomment to trace)
    // g_engine(G_PRINT, "[japlus_proxy] drop client\n", 0,...);
  }
  return g_engine(cmd, a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12);
}

// ---- JA+ command-surface filter (point 2: JA+ am* commands, not vanilla) ----
// JA+ is closed source: validate at the vmMain boundary instead of hex-editing.
// Policy: exec sinks (amvstr/ammap) get single sanitized tokens; passwords are
// compared-never-executed (newlines out only); chat-class allows ';' but never
// CR/LF; everything else denies newline/command-chaining with sane size caps.
static int HasCRLF(const char *s) {
  for (; s && *s; s++) if (*s == '\r' || *s == '\n') return 1;
  return 0;
}
static int HasChaining(const char *s) {
  for (; s && *s; s++) if (*s == ';') return 1;
  return 0;
}
static int IsTokenChars(const char *s) {
  for (; *s; s++) {
    if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
        (*s >= '0' && *s <= '9') || *s == '_' || *s == '-') continue;
    return 0;
  }
  return 1;
}
static int IsMapChars(const char *s) {
  const char *orig = s;
  for (; *s; s++) {
    if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
        (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' ||
        *s == '/' || *s == '.') continue;
    return 0;
  }
  if (orig[0] == '.' && orig[1] == '.' && orig[2] == 0) return 0;
  if (strstr(orig, "../") || strstr(orig, "..\\")) return 0;
  return 1;
}
static int IsNumberChars(const char *s) {
  for (; *s; s++) {
    if ((*s >= '0' && *s <= '9') || *s == '-' || *s == '+' ||
        *s == '.' || *s == ' ' || *s == '\t') continue;
    return 0;
  }
  return 1;
}
static int IsPathChars(const char *s) {
  const char *orig = s;
  for (; *s; s++) {
    if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
        (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' ||
        *s == '/' || *s == '.') continue;
    return 0;
  }
  if (strstr(orig, "../") || strstr(orig, "..\\")) return 0;
  return 1;
}
static int StrCasePrefix(const char *s, const char *prefix, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    char a = s[i], b = prefix[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return 0;
    if (a == 0) return 1;
  }
  return 1;
}
// 1 = block, 0 = allow. Only called for argv(0) starting with "am".
static int JAPlusBlock(const char *cmd, const char *args, int argc,
                       const char *arg1) {
  if (StrCasePrefix(cmd, "amsay", 5) || StrCasePrefix(cmd, "ampsay", 6))
    return (HasCRLF(args) || strlen(args) > 256);
  if (StrCasePrefix(cmd, "amvstr", 6))
    return !(argc == 2 && arg1[0] && strlen(arg1) <= 64 && IsTokenChars(arg1));
  if (StrCasePrefix(cmd, "ammap", 5))
    return !(argc == 2 && arg1[0] && strlen(arg1) <= 64 && IsMapChars(arg1));
  if (StrCasePrefix(cmd, "amlogin", 7))
    return (HasCRLF(args) || strlen(args) > 128);
  if (StrCasePrefix(cmd, "amrename", 8))
    return (!arg1[0] || HasCRLF(arg1) || HasChaining(arg1) || strlen(arg1) > 64);
  if (StrCasePrefix(cmd, "amtele", 6) || StrCasePrefix(cmd, "amtelemark", 10) ||
      StrCasePrefix(cmd, "amorigin", 8))
    return (HasCRLF(args) || HasChaining(args) || strlen(args) > 512 ||
            !IsNumberChars(args));
  if (StrCasePrefix(cmd, "amplayefx", 9) || StrCasePrefix(cmd, "amweather", 9))
    return (!arg1[0] || HasCRLF(args) || HasChaining(args) ||
            strlen(args) > 512 || !IsPathChars(arg1));
  if (StrCasePrefix(cmd, "ampoll", 6))
    return (HasCRLF(args) || HasChaining(args) || strlen(args) > 256);
  return (HasCRLF(args) || HasChaining(args) || strlen(args) > 512);
}

// ---- vanilla basejka command filter (game exploits, cf. OpenJK
// codemp/game/g_cmds.c ClientCommand). 1 = block, 0 = allow.
static int StrCaseEq(const char *a, const char *b, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return 0;
    if (ca == 0) return 1;
  }
  return a[n] == 0;
}
static int VanillaBlock(int clientNum, const char *cmd, const char *args,
                        int argc, const char *arg1, const char *arg2) {
  // cheat-module signature: kick + drop, like the reference proxy
  if (StrCasePrefix(cmd, "jkaDST_", 7)) {
    Proxy_Syscall((intptr_t)G_SEND_SERVER_COMMAND, (intptr_t)-1,
      (intptr_t)"chat \"^3(Anti-Cheat system) cheating detected^7\"",
      0,0,0,0,0,0,0,0,0,0);
    Proxy_Syscall((intptr_t)G_DROP_CLIENT, (intptr_t)clientNum,
      (intptr_t)"(Anti-Cheat system) cheating detected",
      0,0,0,0,0,0,0,0,0,0);
    return 1;
  }
  // gc <slot> with out-of-range slot crashes vanilla gamecode
  if (StrCaseEq(cmd, "gc", 2)) {
    int slot = atoi(arg1);
    if (slot < 0 || slot >= Engine_CvarInt("sv_maxclients"))
      return 1;
  }
  // npc spawn ragnos / saber_droid crashes clients/gamecode
  if (StrCaseEq(cmd, "npc", 3)) {
    char a1[128], a2[128];
    Engine_Argv(1, a1, sizeof(a1));
    Engine_Argv(2, a2, sizeof(a2));
    if (StrCaseEq(a1, "spawn", 5) &&
        (StrCaseEq(a2, "ragnos", 6) || StrCaseEq(a2, "saber_droid", 10)))
      return 1;
  }
  // team follow1/follow2 crash vector
  if (StrCaseEq(cmd, "team", 4) &&
      (StrCaseEq(arg1, "follow1", 7) || StrCaseEq(arg1, "follow2", 7)))
    return 1;
  // callteamvote is useless in basejka and can wedge custom clients
  if (StrCaseEq(cmd, "callteamvote", 12))
    return 1;
  // callvote numeric validation (overflows / stray values crash vote code)
  if (StrCaseEq(cmd, "callvote", 8)) {
    long v;
    if (strlen(arg2) >= 256) return 1;
    v = atol(arg2);
    if (StrCaseEq(arg1, "capturelimit", 12) || StrCaseEq(arg1, "fraglimit", 9)) {
      if (v < 0) return 1;
    } else if (StrCaseEq(arg1, "g_doWarmup", 10)) {
      if (v < 0 || v > 1) return 1;
    } else if (StrCaseEq(arg1, "map_restart", 11)) {
      if (v < 0 || v > 60) return 1;
    } else if (StrCaseEq(arg1, "timelimit", 9)) {
      if (v < 0 || v > 35790) return 1;
    }
    if (HasCRLF(args) || HasChaining(args)) return 1;
    return 0;
  }
  // chat-class: length capped, newlines out, ';' harmless in print path
  if (StrCaseEq(cmd, "say", 3) || StrCaseEq(cmd, "say_team", 8) ||
      StrCaseEq(cmd, "tell", 4))
    return (HasCRLF(args) || strlen(args) > 256);
  (void)argc;
  return (HasCRLF(args) || HasChaining(args));
}

// ---- engine -> game direction: vmMain filter ----
static int ShouldBlockClientCommand(int clientNum) {
  char arg0[128], arg1[128], arg2[256], args[1024];
  int argc, i, len;
  Engine_Argv(0, arg0, sizeof(arg0));
  argc = Engine_Argc();
  Engine_Argv(1, arg1, sizeof(arg1));
  Engine_Argv(2, arg2, sizeof(arg2));
  args[0] = 0; len = 0;
  for (i = 1; i < argc && len < (int)sizeof(args) - 1; i++) {
    char tok[256];
    Engine_Argv(i, tok, sizeof(tok));
    if (i > 1 && len < (int)sizeof(args) - 1) args[len++] = ' ';
    {
      size_t tlen = strlen(tok);
      size_t room = sizeof(args) - 1 - (size_t)len;
      if (tlen > room) tlen = room;
      memcpy(args + len, tok, tlen);
      len += (int)tlen;
    }
  }
  args[len] = 0;
  if (StrCasePrefix(arg0, "am", 2))
    return JAPlusBlock(arg0, args, argc, arg1);
  return VanillaBlock(clientNum, arg0, args, argc, arg1, arg2);
}

static int UserinfoLooksEvil(const char *s) {
  if (!s) return 1;
  size_t n = strlen(s);
  if (n == 0 || n >= 1024) return 1; // MAX_INFO_STRING overflow attempt
  // overlong single key/value is the classic userinfo bomb; engine usually
  // enforces, but JA+ has had bypasses — belt and suspenders.
  return 0;
}

// ---- minimal info-string helpers (Q3 \key\value format) ----
static int Info_Value(const char *info, const char *key, char *out,
                      size_t outlen) {
  size_t klen;
  if (!info || !key || outlen == 0) return 0;
  out[0] = 0;
  klen = strlen(key);
  while (*info == '\\') {
    const char *k = info + 1, *kend, *v, *vend;
    size_t n;
    kend = strchr(k, '\\');
    if (!kend) return 0;
    v = kend + 1;
    vend = strchr(v, '\\');
    if (!vend) vend = v + strlen(v);
    n = (size_t)(kend - k);
    if (n == klen) {
      size_t i;
      for (i = 0; i < n; i++) {
        char a = k[i], b = key[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) break;
      }
      if (i == n) {
        size_t vlen = (size_t)(vend - v);
        if (vlen > outlen - 1) vlen = outlen - 1;
        memcpy(out, v, vlen);
        out[vlen] = 0;
        return 1;
      }
    }
    info = (*vend) ? vend : vend;
    if (!*info) return 0;
  }
  return 0;
}
static void Info_Set(char *info, size_t infolen, const char *key,
                     const char *value) {
  char tmp[2048];
  size_t klen, pos = 0;
  const char *p;
  if (!info || !key) return;
  klen = strlen(key);
  tmp[0] = 0;
  p = info;
  while (*p == '\\') { // copy every pair except `key`
    const char *k = p + 1, *kend, *v, *vend;
    size_t n;
    kend = strchr(k, '\\');
    if (!kend) break;
    v = kend + 1;
    vend = strchr(v, '\\');
    if (!vend) vend = v + strlen(v);
    n = (size_t)(kend - k);
    {
      size_t i;
      int match = (n == klen);
      for (i = 0; match && i < n; i++) {
        char a = k[i], b = key[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) match = 0;
      }
      if (!match) {
        size_t pair = (size_t)(vend - p);
        if (pos + pair < sizeof(tmp) - 1) {
          memcpy(tmp + pos, p, pair);
          pos += pair;
          tmp[pos] = 0;
        }
      }
    }
    p = (*vend) ? vend : vend;
    if (!*p) break;
  }
  if (value && *value) { // append new pair if it fits (MAX_INFO_STRING)
    size_t need = 1 + klen + 1 + strlen(value);
    if (pos + need < infolen - 1 && pos + need < sizeof(tmp) - 1) {
      tmp[pos++] = '\\';
      memcpy(tmp + pos, key, klen); pos += klen;
      tmp[pos++] = '\\';
      strcpy(tmp + pos, value);
    }
  }
  strncpy(info, tmp, infolen - 1);
  info[infolen - 1] = 0;
}
static int IsAsciiClean(const char *s) {
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 32 || c > 126) return 0;
  }
  return 1;
}
// C port of the reference name cleaner: strips controls/spoof chars,
// collapses runs, never returns empty. Returns 1 if `out` differs.
static void CleanName(const char *in, char *out, size_t outlen) {
  size_t pos = 0;
  int colorless = 0;
  while (*in == ' ' || *in == '*') in++; // leading spaces/skips
  for (; *in && pos + 1 < outlen && pos + 1 < 64; in++) {
    unsigned char c = (unsigned char)*in;
    if (c < 0x20) continue;
    switch (c) { // extended bytes that break renderers / listings
      case 0x81: case 0x8D: case 0x8F: case 0x90:
      case 0x9D: case 0xA0: case 0xAD: continue;
    }
    if (in[0] && in[1] && in[2] &&
        ((in[0] == ' ' && in[1] == ' ' && in[2] == ' ') ||
         (in[0] == '@' && in[1] == '@' && in[2] == '@')))
      continue;
    out[pos++] = *in;
    if (!(pos > 1 && out[pos - 2] == '^' &&
          ((in[0] >= '0' && in[0] <= '9') || in[0] == '#')))
      colorless++;
  }
  out[pos] = 0;
  if (pos == 0 || colorless == 0) strncpy(out, "Padawan", outlen - 1);
  out[outlen - 1] = 0;
}
// Sanitize JA+ userinfo in place. Returns -1 = kick, 0 = clean, 1 = fixed.
static int SanitizeUserinfo(char *ui, size_t uilen, char *kickReason,
                            size_t klen) {
  char val[512], fixed[512];
  int changed = 0;
  if (Info_Value(ui, "model", val, sizeof(val))) {
    int bad = 0;
    if (StrCaseEq(val, "darksidetools", 12)) {
      strncpy(kickReason, "cheating detected", klen - 1);
      kickReason[klen - 1] = 0;
      return -1;
    }
    if (StrCaseEq(val, "jedi_/red", 9) || StrCaseEq(val, "jedi_/blue", 10) ||
        StrCaseEq(val, "rancor", 6) || StrCaseEq(val, "wampa", 5) ||
        !IsAsciiClean(val) || strlen(val) >= 64)
      bad = 1;
    if (bad) { Info_Set(ui, uilen, "model", "kyle"); changed = 1; }
  }
  if (Info_Value(ui, "forcepowers", val, sizeof(val))) {
    size_t n = strlen(val), i;
    int seps = 0, bad = 0;
    if (n < 22 || n > 24) bad = 1;
    else {
      for (i = 0; i < n && !bad; i++) {
        char c = val[i];
        if (c != '-' && (c < '0' || c > '9')) bad = 1;
        else if (c == '-') {
          if (i < 1 || i > 5) bad = 1;
          else if (val[i - 1] == '-') bad = 1;
          else seps++;
        }
      }
      if (seps != 2) bad = 1;
    }
    if (bad) {
      Info_Set(ui, uilen, "forcepowers", "7-1-030000000000003332");
      changed = 1;
    }
  }
  if (Info_Value(ui, "name", val, sizeof(val))) {
    CleanName(val, fixed, sizeof(fixed));
    if (strcmp(val, fixed) != 0) { Info_Set(ui, uilen, "name", fixed); changed = 1; }
  }
  // Spawn-adjacent keys: attacker-controlled file/class lookups at spawn.
  // siegeclass -> class lookup by name; saber1/saber2 -> hilt file loads.
  // Overlong values / traversal sequences crash parsers and clients, so
  // confine charset and drop the key (game default applies) when dirty.
  if (Info_Value(ui, "siegeclass", val, sizeof(val))) {
    if ((val[0] && !IsTokenChars(val)) || strlen(val) >= 64 || HasCRLF(val)) {
      Info_Set(ui, uilen, "siegeclass", "");
      changed = 1;
    }
  }
  if (Info_Value(ui, "saber1", val, sizeof(val))) {
    if (!IsPathChars(val) || strlen(val) >= 64 || HasCRLF(val)) {
      Info_Set(ui, uilen, "saber1", "");
      changed = 1;
    }
  }
  if (Info_Value(ui, "saber2", val, sizeof(val))) {
    if (!IsPathChars(val) || strlen(val) >= 64 || HasCRLF(val)) {
      Info_Set(ui, uilen, "saber2", "");
      changed = 1;
    }
  }
  return changed;
}

// Exported entry points — these names MUST match what jampded looks up.
#if defined(__linux__)
__attribute__((visibility("default")))
#endif
void QDECL dllEntry(syscall_t engine) {
  g_engine = engine;
  LoadReal();
  if (real_dllEntry)
    // Cast: our fixed 13-arg trampoline is ABI-compatible with the
    // variadic syscall_t on x86 cdecl (all args on the stack).
    real_dllEntry((syscall_t)Proxy_Syscall); // game now calls US for engine services
  else
    fprintf(stderr, "[japlus_proxy] no real dllEntry\n");
}

#if defined(__linux__)
__attribute__((visibility("default")))
#endif
intptr_t QDECL vmMain(int cmd,
  intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4,
  intptr_t a5, intptr_t a6, intptr_t a7, intptr_t a8, intptr_t a9,
  intptr_t a10, intptr_t a11)
{
  LoadReal();
  if (!real_vmMain) {
    fprintf(stderr, "[japlus_proxy] no real vmMain\n");
    return -1;
  }

  switch (cmd) {
    case GAME_CLIENT_COMMAND:
      if (ShouldBlockClientCommand((int)a0)) {
        Proxy_Syscall((intptr_t)G_PRINT, (intptr_t)"[japlus_proxy] blocked client command\n",
          0,0,0,0,0,0,0,0,0,0,0);
        return 0;
      }
      break;
    case GAME_CLIENT_USERINFO_CHANGED: {
      char ui[2048], kick[128];
      int rc;
      Engine_GetUserinfo((int)a0, ui, sizeof(ui));
      if (UserinfoLooksEvil(ui)) {
        Proxy_Syscall((intptr_t)G_DROP_CLIENT, (intptr_t)a0,
          (intptr_t)"bad userinfo", 0,0,0,0,0,0,0,0,0,0);
        return 0; // don't forward evil userinfo into game logic
      }
      rc = SanitizeUserinfo(ui, sizeof(ui), kick, sizeof(kick));
      if (rc < 0) { // cheat signature in model key
        Proxy_Syscall((intptr_t)G_SEND_SERVER_COMMAND, (intptr_t)-1,
          (intptr_t)"chat \"^3(Anti-Cheat system) cheating detected^7\"",
          0,0,0,0,0,0,0,0,0,0);
        Proxy_Syscall((intptr_t)G_DROP_CLIENT, (intptr_t)a0,
          (intptr_t)"(Anti-Cheat system) cheating detected",
          0,0,0,0,0,0,0,0,0,0);
        return 0;
      }
      if (rc > 0) Engine_SetUserinfo((int)a0, ui); // fixed model/force/name
      break; // forward (sanitized) userinfo into the real game
    }
    case GAME_CLIENT_CONNECT: {
      // a0=clientNum, a1=firstTime, a2=isBot — could add name/IP bans here.
      // Return value is denial string ptr (NULL = allow); to deny, return
      // a static string instead of calling through. Keep permissive for now.
      break;
    }
    case GAME_CLIENT_BEGIN: {
      // Spawn entry (engine -> ClientBegin -> ClientSpawn). The engine always
      // sends a valid slot, but a hostile/modded engine or a corrupted packet
      // path handing us garbage here would OOB-index g_clients[] in the game.
      // Cheap range gate; MAX_CLIENTS is 32 in JKA MP.
      int cl = (int)a0;
      if (cl < 0 || cl >= 32) {
        Proxy_Syscall((intptr_t)G_PRINT,
          (intptr_t)"[japlus_proxy] blocked begin with bad clientNum\n",
          0,0,0,0,0,0,0,0,0,0,0);
        return 0;
      }
      break;
    }
    default:
      break;
  }

  return real_vmMain(cmd, a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11);
}
