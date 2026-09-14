# japm — JAPlus Manager

Starts and babysits JA+ dedicated servers with japlusproxy protection.
Derived from vmb2m (Valzhar's MBII Manager): same workflow — per-instance
JSON configs, generated `server.cfg`, watchdog with crash auto-restart —
retargeted at stock `linuxjampded` + JA+, with the MBII updater/smod
wiring removed and proxy deploy/verify built in.

Linux only (the proxy is a Linux i386 build).

## Setup

```sh
./install.sh                    # python3, 32-bit libs, screen, `japm` shortcut
cd ../proxy && make             # build japlusproxy.so
cp configs/example.json configs/my_server.json
nano configs/my_server.json     # ja_path, host_name, rcon_password, maps
```

`ja_path` is the GameData dir holding `linuxjampded` + `jampgamei386.so`.
Set it once in `japm.conf` (`[locations]`) — it applies to all instances.
Per-instance `server.ja_path` overrides it when non-empty; empty means
"use the global". After that: `JAPLUS_HOME` env, then the current
directory (so running japm from the server folder works with no config),
then fixed auto-detect paths.

## Commands

| Action | Command |
|---|---|
| Check proxy setup | `japm my_server proxy` |
| Start server | `japm my_server start` |
| Stop server | `japm my_server stop` |
| Restart server | `japm my_server restart` |
| Check status | `japm my_server status` |
| List instances | `japm --list` |

Foreground mode for debugging: `JAPM_FG=1 japm my_server start`.

## What start does

1. Resolves GameData, generates `<name>-server.cfg` (+ map lists) into
   `GameData/japlus/`.
2. **Proxy preflight**: checks the built `japlusproxy.so` is a 32-bit ELF
   and a game lib exists. Refuses to start (when proxy enabled) if broken.
3. **Deploys the wrapper**: preserves the original as
   `japlus_real_i386.so` once, copies `japlusproxy.so` over
   `jampgamei386.so`, exports `JAPLUS_REAL` (+ `JAPLUS_NO_HOOKS=1` if
   `proxy.no_hooks`).
4. Launches `linuxjampded` (`+set dedicated 2`, `net_port`, `fs_homepath`
   → GameData, `fs_game japlus`, `+exec`) under `screen jap_<name>`,
   waits for it, connects RCON, starts native plugins + log watcher.
5. **Verifies the proxy**: engine stdout+stderr is captured to
   `<name>-engine.log` (never discarded), and the fresh log is tailed for
   `[japlus_proxy] hooked …` lines — warns on `SKIPPED` (wrong JA+ build)
   or silence (wrapper didn't load).
6. Supervises: crash auto-restart (5 tries), scheduled restart via
   `restart_every_hours`, standalone plugin respawn.

## Instance config

`configs/<name>.json` (see `example.json`):

- `server`: `host_name`, `port`, `restart_every_hours`, `ja_path`,
  `engine`, `fs_game` (default `japlus`).
- `proxy`: `enabled`, `source` (empty = `../proxy/japlusproxy.so` from the
  repo), `no_hooks`, `verify_timeout`.
- `plugins`: `automessage` (rotating `svsay`), `vpnmonitor` (needs an
  iphub `apikey`), `rtvrtm` (`enabled: false` default — see below).
- `security`: `rcon_password`, `server_password`.
- `game`: `starting_map`, `gametype` (stock values: 0 FFA, 1 Holocron,
  2 JediMaster, 3 Duel, 4 PowerDuel, 6 Team, 7 Siege, 8 CTF), `maxclients`,
  `timelimit`, `fraglimit`, `duellimit`,
  `message_of_the_day`, plus free-form `cvars` appended to server.cfg.
- `maps`: `primary` / `secondary` lists (map files for voting plugins).

Global defaults live in `japm.conf`.

## Plugins

Enable under `"plugins"` (`true` = defaults, `{ }` = custom, omit/`false`
= off). Native plugins get RCON + log events; `plugins/<name>/<name>.py`
scripts run standalone (currently only rtvrtm ships one).

- `automessage` — rotating chat lines via `svsay`. Works as-is.
- `vpnmonitor` — iphub VPN/proxy check on connect, kick/ban. Works as-is
  once `apikey` is set.
- `rtv` — rock-the-vote rewritten for basejka: `!rtv` / `!unrtv`,
  `!nominate <map>`, numbered votes (`!1`-`!5`), nomination limits,
  recently-played blocking, success/fail cooldowns, optional map extend.
  Winners switch via `g_gametype` + `map` (per-map overrides in
  `map_gametypes`). Map pool comes from your instance `maps` lists;
  gametypes are stock (0 FFA, 1 Holocron, 2 JediMaster, 3 Duel,
  4 PowerDuel, 6 Team, 7 Siege, 8 CTF).

## Layout

```
japm.py                 # boot, process control, proxy deploy/verify
japm.conf               # global defaults (paths, engine, fs_game)
configs/example.json    # instance template (only example is versioned)
configs/server.template # server.cfg template ([bracket] keys)
plugins/base.py         # native plugin base class
plugins/event_types.py  # chat/kill/connect/map event types
plugins/manager.py      # native plugin loader + RCON API
plugins/automessage.py  # rotating messages (native)
plugins/vpnmonitor/     # iphub VPN check (native)
plugins/rtv.py          # rock-the-vote for basejka (native)
pids/                   # runtime PID files (gitignored)
```

`configs/*.json` (except the example) are gitignored — they hold rcon
passwords.
