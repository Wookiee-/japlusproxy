# japlusproxy

Trampoline middleman proxy for the closed-source JA+ `jampgamei386.so`
(Jedi Academy, Quake 3 engine). It sits between `jampded` and the real game
lib, filtering both directions and inline-hooking verified internals —
no hex-editing of the original binary.

## How it works

```
jampded  --dllEntry/vmMain-->  japlusproxy.so  --dllEntry/vmMain-->  real JA+ lib
   ^                               |                                      |
   |________ syscall filter ________|________ inline hooks (patch.c) ______|
```

The game module ABI here is **legacy Quake 3** (`dllEntry(syscall)` +
`vmMain(cmd, …)`), *not* OpenJK `GetModuleAPI` — verified against the
binary (exports `vmMain` + `dllEntry`, no `GetModuleAPI`).

- **Engine → game** (`vmMain` filter in `proxy/proxy.c`): `CLIENT_COMMAND`
  (JA+ `am*` policy + vanilla vectors), `USERINFO_CHANGED` (model /
  forcepowers / name / siegeclass / saber sanitizing with `SetUserinfo`
  write-back), `CLIENT_BEGIN` (slot range gate for the spawn path).
- **Game → engine** (syscall filter): `G_SEND_SERVER_COMMAND` length cap,
  `G_SET_CONFIGSTRING` index/length cap, `G_GET_USERCMD` sanitizing
  (`forcesel` OOB/`FP_LEVITATION` → `0xFF`, `angles[ROLL]` → 0),
  `G_GET_ENTITY_TOKEN` NUL-guarantee + `MAX_TOKEN_CHARS` truncation.
- **Inline hooks** (`proxy/patch.c`): trampoline (`0xE9` rel32 + NOP pad,
  instruction-boundary-aware) into the real lib for what the boundary
  can't see. Currently armed: `BG_SiegeFindClassByName` (NULL + ≥64-char
  guard). All targets byte-interlocked (see below).

## Repo layout

| Path | Purpose |
|---|---|
| `proxy/proxy.c`, `patch.c/h`, `Makefile` | **Our code. Build this.** |
| `jampgamei386.so` | Your original JA+ lib (reference + audit source, never modified) |
| `OpenJK/` | Reference source for IDs/logic (`codemp/game/g_public.h`, `g_main.c`, `g_syscalls.c`, `bg_saga.c`, `qcommon/q_shared.h`) |
| `JKA_YBEProxy/` | Reference only (vanilla-JKA proxy). Do not edit; policy was ported from it where applicable |

## Build (Linux i386 host — not Windows)

```sh
sudo apt install gcc-multilib libc6-dev-i386
cd proxy && make        # -> japlusproxy.so
```

## Deploy next to jampded

**A) Stock `jampded`** (loads `jampgamei386.so` by name):

```sh
mv jampgamei386.so japlus_real_i386.so   # original JA+ lib
cp proxy/japlusproxy.so jampgamei386.so  # wrapper takes the name
```

**B) Engine loading `japlusproxy.so`** — drop it next to the untouched
`jampgamei386.so`.

Real-lib resolution order: `$JAPLUS_REAL` → `./japlus_real_i386.so` →
`./jampgamei386.so`. Any candidate resolving to the proxy itself is
skipped (safe under both layouts). `JAPLUS_NO_HOOKS=1` disarms inline
hooks (boundary filters stay on).

## Verify on first load

Watch stderr / the server log for:

```
[japlus_proxy] real base=0x… vmMain=0x…
[japlus_proxy] target BG_SiegeFindClassByName verified
[japlus_proxy] hooked BG_SiegeFindClassByName@0x… tramp=0x…
```

`SKIPPED` on a target means the byte interlock mismatched (wrong JA+
build) — that hook stays off. Before going live: connect, chat, use an
`am*` command, pick a siege class, die/respawn, vote. Then try something
evil (`amsay` with a newline, `amvstr rconpassword`, overlong model) and
confirm it is dropped/logged while legit play is unaffected.

## Protection layers (where to add more)

1. **Boundary filters** (`proxy/proxy.c`): `JAPlusBlock()` for `am*`
   (exec sinks `amvstr`/`ammap` = single sanitized token; `amlogin` =
   length-only so real passwords keep working; `amsay`/`ampsay` = chat
   rules), `VanillaBlock()` for basejka commands (`gc`, `npc spawn
   ragnos/saber_droid`, `team follow1/2`, `callteamvote`, `callvote`
   ranges), `SanitizeUserinfo()` for `model`/`forcepowers`/`name`/
   `siegeclass`/`saber1`/`saber2`. Add new signatures here first —
   no binary knowledge needed.
2. **Inline hooks** (`proxy/patch.c`): for internals the boundary can't
   reach. Pick the target from the table, confirm its bytes, write a
   detour that preserves behavior for legit inputs.

## Finding new offsets in the binary

The `.so` ships an unstripped `.symtab` — exact function addresses, no
guessing. With LLVM tools (e.g. Android NDK `llvm-readelf` /
`llvm-objdump`):

```sh
R=llvm-readelf; O=llvm-objdump; S=jampgamei386.so
$R --symbols -W $S | grep -E "ClientSpawn|Cmd_amlogin_f|BG_SiegeFindClass"
$R -r -W $S | grep "^001a...."          # relocations = resolved call targets
$O -d --disassemble-symbols=Cmd_amvstr_f -M intel $S
```

Relocation entries (`R_386_PC32`) name the exact import each call site
hits; string pushes resolve to `.rodata`/`.data` (file offset == vaddr
here). Audit rules that paid off: list `UND` imports for
`strcpy|strcat|sprintf|system|gets` (this binary has none — only
`strncpy`/`sscanf`/`vsnprintf`), map every `sscanf` site to its function,
check `%s` without width and `strncpy` without a following NUL store.

Confirmed vaddrs (link-time; runtime = base + vaddr):

```
ClientSpawn 0x122DD4 | ClientCommand 0x1B2E06 | ClientUserinfoChanged 0x11F8FE
SetTeam 0x19F338 | Cmd_SiegeClass_f 0x1A041A | G_ParseSpawnVars 0x14E4A2
Cmd_amvstr_f 0x1AA4BC | Cmd_amlogin_f 0x1A470A | Cmd_ammap_f 0x1A5A56
BG_SiegeFindClassByName 0x10DB34
```

## Safety notes

- Hooks only plant when the recorded first bytes match; `prefix_len`
  must end on an instruction boundary (verified per target with objdump
  — blind 5-byte hooks can split instructions).
- Boundary filters fail closed (drop + log); userinfo sanitizing
  rewrites only dirty keys so legit names/models/sabers pass through
  untouched.
- This project links no OpenJK/YBEProxy code; they are reference only.
