# japlusproxy

Keeps a JA+ (Jedi Academy) game server safe from known crash and cheat
exploits — without modifying the original JA+ files.

## What it does

If you run a JA+ server, attackers can crash it or cheat on it by sending
cleverly crafted player names, chat messages, admin commands, character
models, and map data. The original JA+ mod is closed-source and no longer
patched, so these holes stay open forever.

This project is a small protective wrapper. It sits between the server
program (`linuxjampded`) and the JA+ game logic (`jampgamei386.so`) and:

- **Blocks malicious commands** — chat/admin commands containing hidden
  extra commands, line breaks, or password-stealing tricks are dropped and
  logged. Normal chat and play are unaffected.
- **Kicks cheat mods** — clients running known cheat modules or crash
  models are removed with an "Anti-Cheat" message.
- **Cleans up player data** — overlong or garbage names, models, Force
  powers, siege classes, and lightsaber choices are reset to safe defaults
  instead of crashing the server.
- **Caps oversized data** — huge server messages and broken map data that
  would overflow fixed-size buffers are truncated or rejected.
- **Patches risky internals** — one dangerous lookup inside JA+ is guarded
  directly (bad input returns "not found" instead of crashing). More guards
  can be added the same way.
- **Slows packet floods** — connectionless packets (`getstatus` / `getinfo`
  / `connect` / `rcon` spam, the classic reflection-flood vector) are
  throttled per-IP and globally with a leaky bucket ported from OpenJK.
  The stock engine has no such limit. Local traffic (127.0.0.1) is exempt.
- **Kills the `donedl` respawn cheat** — the engine's download-complete
  handler is neutered while `sv_allowdownload` is 0 (the default), so
  clients can't force gamestates/respawns through the download handshake.
  Enable downloads and the handshake passes through untouched.
- **Throttles `rcon` brute force** — password guesses are limited per IP
  (short burst, then 1 per 2 s) before the password is even tested. Local
  admin tools (127.0.0.1) are exempt.

Legit players notice nothing. Attackers get dropped or logged.

## Who needs it

Anyone hosting a public JA+ server on Linux. If your server is local-only
with trusted friends, you don't need this.

## Install

You need a 32-bit-capable Linux host (not Windows) to build:

```sh
sudo apt install gcc-multilib libc6-dev-i386 cmake
cd proxy && make        # -> japlusproxy.so
```

Then put the wrapper next to your server. Pick one layout:

**A) Standard server** (expects a file named `jampgamei386.so`):

```sh
mv jampgamei386.so japlus_real_i386.so   # keep the original JA+ file
cp proxy/japlusproxy.so jampgamei386.so  # wrapper takes its name
```

**B) Custom setup** — just drop `japlusproxy.so` next to your untouched
`jampgamei386.so`. The wrapper finds the real file by itself.

Optional settings:

- `JAPLUS_REAL=/path/to/real.so` — point at the original JA+ file directly.
- `JAPLUS_NO_HOOKS=1` — turn off the internal patches (command and data
  filtering stays on).

## Check it's working

Start the server and look at its startup output. You should see:

```
[japlus_proxy] real base=0x… vmMain=0x…
[japlus_proxy] target BG_SiegeFindClassByName verified
[japlus_proxy] hooked BG_SiegeFindClassByName@0x… tramp=0x…
[japlus_proxy] target SV_ConnectionlessPacket  verified
[japlus_proxy] hooked SV_ConnectionlessPacket@0x… tramp=0x…
[japlus_proxy] protections: filters=on hooks_armed=4/4
```

Then smoke-test before going live: connect, chat, use an admin command,
pick a siege class, die and respawn, call a vote. Then try something evil
(a chat message with a line break in it, an admin variable read of
`rconpassword`, an overlong model name) and confirm it is blocked and
logged while normal play works.

If a line says `SKIPPED` instead of `verified`, that patch didn't match
your JA+ build and stays off — the rest still protects you.

## What's in this folder

| Path | Purpose |
|---|---|
| `proxy/` | **Our code. Build this.** `proxy.c` (filters), `patch.c/h` (internal patches), `Makefile`, `CMakeLists.txt`, vendored 32-bit toolchain |
| `japm/` | JAPlus Manager: starts/stops/supervises servers, deploys + verifies the proxy (`japm <name> start`) |
| `jampgamei386.so` | Your original JA+ file (never modified, never committed) |
| `linuxjampded`, `libcxa.so.1` | Your server program + its helper library (never modified, never committed) |
| `OpenJK/`, `JKA_YBEProxy/` | Reference source used to understand the engine (not built, not committed) |

The reference folders and local binaries are all listed in `.gitignore` —
only `proxy/` and this README are versioned.

## For developers

### Architecture

```
jampded --dllEntry/vmMain--> japlusproxy.so --dllEntry/vmMain--> real JA+ lib
   ^                              |                                     |
   |_______ syscall filter ________|_______ internal patches ____________|
              (proxy.c)                      (patch.c)
```

The game module uses the legacy Quake 3 interface (`dllEntry(syscall)` +
`vmMain(cmd, …)`), not OpenJK's `GetModuleAPI` — confirmed: the binary
exports `vmMain` + `dllEntry` and no `GetModuleAPI`. Message IDs come from
`OpenJK/codemp/game/g_public.h`; the player-input layout from
`codemp/qcommon/q_shared.h`.

- **Engine → game** (`vmMain` filter): client commands (JA+ `am*` policy +
  vanilla crash vectors), player-info changes (sanitized with `SetUserinfo`
  write-back), spawn-entry slot check.
- **Game → engine** (syscall filter): server-message length cap,
  config-string index/length cap, player-input sanitizing, map-token
  truncation.
- **Internal patches** (`patch.c`): short jumps (`0xE9` rel32 + NOP pad,
  placed on instruction boundaries) redirecting risky JA+ functions through
  guards. Currently armed: `BG_SiegeFindClassByName` (NULL and ≥64-char
  guard). Every patch checks the expected first bytes before planting, so a
  wrong JA+ build can never be patched by mistake.

### Where to add more

1. **Boundary filters** (`proxy/proxy.c`) — add new attack signatures here
   first. No binary knowledge needed: `JAPlusBlock()` for `am*` commands,
   `VanillaBlock()` for base-game commands, `SanitizeUserinfo()` for
   player data.
2. **Internal patches** (`proxy/patch.c`) — for risky code the boundary
   can't see. Pick a target from the table below, confirm its bytes with a
   disassembler, write a guard that passes legit input through unchanged.

### Finding offsets in the game lib

`jampgamei386.so` still has its symbol table, so function addresses are
exact — no guessing. With LLVM tools (e.g. Android NDK `llvm-readelf` /
`llvm-objdump`):

```sh
R=llvm-readelf; O=llvm-objdump; S=jampgamei386.so
$R --symbols -W $S | grep -E "ClientSpawn|Cmd_amlogin_f|BG_SiegeFindClass"
$R -r -W $S | grep "^001a...."          # relocations = resolved call targets
$O -d --disassemble-symbols=Cmd_amvstr_f -M intel $S
```

Confirmed addresses (link-time; runtime = base + address):

```
ClientSpawn 0x122DD4 | ClientCommand 0x1B2E06 | ClientUserinfoChanged 0x11F8FE
SetTeam 0x19F338 | Cmd_SiegeClass_f 0x1A041A | G_ParseSpawnVars 0x14E4A2
Cmd_amvstr_f 0x1AA4BC | Cmd_amlogin_f 0x1A470A | Cmd_ammap_f 0x1A5A56
BG_SiegeFindClassByName 0x10DB34
```

Audit notes that paid off: the lib imports no `strcpy`/`strcat`/`sprintf`/
`system`/`gets` (only `strncpy`/`sscanf`/`vsnprintf`); the 2
player-reachable `sscanf` sites use `%f` with return-value checks (safe);
`amvstr`/`ammap` already gate on admin rights and block password variables
(the boundary filter adds defense in depth).

### Notes on the engine binary (`linuxjampded`)

Now triaged (all 32-bit x86, GCC 2.95.3 era, original stock build):

- **Stripped** — no symbol table (only ~142 import entries), so engine
  internals need pattern-based targets, unlike the game lib.
- **Not position-independent** (fixed load base `0x08048000`), so once a
  target is found its address is stable.
- Runs the legacy `vmMain`/`dllEntry` loading (`Sys_LoadDll` strings
  confirm it) — the wrapper design matches.
- Imports the classic unsafe functions the game lib avoids (`sprintf`,
  `vsprintf`, `sscanf`, `strtok`, `strncat`) — worth auditing around the
  `rcon`, `download`, challenge-auth, and `Cvar_/Com_/SV_` code paths.
- The wrapper already runs **inside the engine's process** (loaded via
  `dlopen`), so engine-side patches reuse the same `mprotect` + jump
  technique. First one armed: `SV_ConnectionlessPacket` (absolute
  `0x08056D64`, prologue + `getstatus`-string interlock) with OpenJK's
  leaky bucket — per-IP (default 1/sec, burst 10) plus a global backstop
  (default 1000/sec), tunable via `sv_maxOOBRateIP` / `sv_maxOOBRate` if
  those cvars exist (absent on stock → compiled defaults apply, cached
  every 5 s). Because engine `.text` sits ~3.8 GB from our `.so`, this
  hook uses absolute jumps (`HookJumpAbs`: `push+ret` / `mov+jmp`), not
  rel32.

### Safety rules

- Patches only plant when recorded first bytes match; hook lengths are
  additionally cross-checked at runtime with a vendored x86 length
  disassembler (`proxy/hde32.*`, HDE32 by Patkov via JKA_YBEProxy) — a
  length that would split an instruction fails closed with
  `length mismatch`. Rel32 jumps are preferred; absolute jumps cover far
  targets outside the 2 GB rel32 range.
- Filters fail closed (drop + log); player-data sanitizing rewrites only
  dirty keys so legit names/models/sabers pass through untouched.
- This project links no OpenJK/YBEProxy code; they are reference only.
