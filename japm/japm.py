#!/usr/bin/env python3
"""
japm -- JAPlus Manager. Config, boot, process control and auto-restart for
JA+ (Jedi Academy) dedicated servers, with first-class japlusproxy support.

Derived from vmb2m (Valzhar's MBII Manager): MBII updater/smod/voting
wiring removed, engine + paths retargeted at stock linuxjampded + JA+,
proxy deploy/verify added. Linux-first (the proxy is Linux i386 only).
"""

import configparser
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

IS_WINDOWS = sys.platform.startswith("win")
IS_LINUX = sys.platform.startswith("linux")

ENGINE_BIN = "linuxjampded"
GAME_LIB = "jampgamei386.so"
REAL_LIB_NAME = "japlus_real_i386.so"
FS_GAME = "japlus"


class C:
    RED = "\033[91m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    CYAN = "\033[96m"
    END = "\033[0m"


def ok(msg):
    print("  %s[OK]%s %s" % (C.GREEN, C.END, msg))


def warn(msg):
    print("  %s[WARN]%s %s" % (C.YELLOW, C.END, msg))


def fail(msg):
    print("  %s[ERROR]%s %s" % (C.RED, C.END, msg))


def info(msg):
    print("  %s%s%s" % (C.CYAN, msg, C.END))


if not IS_WINDOWS and not IS_LINUX:
    print("[WARN] Unsupported OS: %s, Linux features may not work" % sys.platform)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from plugins.manager import PluginManager
from plugins import event_types as events

BASE = Path(__file__).resolve().parent
PID_DIR = BASE / "pids"
# japlusproxy lives next to japm in the repo (../proxy). An installed copy
# can be pointed at via the instance "proxy.source" setting instead.
REPO_PROXY_SO = BASE.parent / "proxy" / "japlusproxy.so"

RTVRTM_FIELD_MAP = {
    "log": "logfile", "mbii folder": "MBII_Folder",
    "address": "address", "bind": "bindaddr",
    "password": "rcon_pwd", "flood protection": "flood_protection",
    "use say only": "use_say_only", "name protection": "name_protection",
    "default game": "default_game", "clean log": "clean_log",
    "admin voting": "admin_voting", "admin minimum votes": "admin_minimum_votes",
    "admin skip voting": "admin_skip_voting",
    "roundlimit": "roundlimit", "timelimit": "timelimit",
    "limit voting": "limit_voting", "limit minimum votes": "limit_minimum_votes",
    "limit extend": "limit_extend", "limit successful wait time": "limit_s_wait_time",
    "limit failed wait time": "limit_f_wait_time", "limit skip voting": "limit_skip_voting",
    "limit second turn": "limit_second_turn", "limit change immediately": "limit_change_immediately",
    "rtv": "rtv", "rtv rate": "rtv_rate", "rtv voting": "rtv_voting",
    "rtv minimum votes": "rtv_minimum_votes", "rtv extend": "rtv_extend",
    "rtv successful wait time": "rtv_s_wait_time", "rtv failed wait time": "rtv_f_wait_time",
    "rtv skip voting": "rtv_skip_voting", "rtv second turn": "rtv_second_turn",
    "rtv change immediately": "rtv_change_immediately",
    "automatic maps": "automatic_maps", "maps": "maps", "secondary maps": "secondary_maps",
    "pick secondary maps": "pick_secondary_maps", "map priority": "map_priority",
    "nomination type": "nomination_type", "enable recently played maps": "enable_recently_played",
    "rtm": "rtm", "mode priority": "mode_priority", "rtm rate": "rtm_rate",
    "rtm voting": "rtm_voting", "rtm minimum votes": "rtm_minimum_votes",
    "rtm extend": "rtm_extend", "rtm successful wait time": "rtm_s_wait_time",
    "rtm failed wait time": "rtm_f_wait_time", "rtm skip voting": "rtm_skip_voting",
    "rtm second turn": "rtm_second_turn", "rtm change immediately": "rtm_change_immediately",
}


def load_global_config():
    """Read japm.conf for global defaults (paths, engine, game)."""
    conf = BASE / "japm.conf"
    if not conf.exists():
        return {}
    cfg = configparser.ConfigParser()
    cfg.read(str(conf))
    result = {}
    if cfg.has_section("locations"):
        for key in ("ja_path", "config_path"):
            val = cfg.get("locations", key, fallback="").strip()
            if val:
                result[key] = val
    if cfg.has_section("dedicated"):
        for key in ("engine", "fs_game"):
            val = cfg.get("dedicated", key, fallback="").strip()
            if val:
                result[key] = val
    return result


GLOBAL_CFG = load_global_config()
CONFIG_DIR = Path(GLOBAL_CFG.get("config_path", "")) if GLOBAL_CFG.get("config_path") else BASE / "configs"


def merge_config(instance_cfg):
    """Precedence: instance server.* (when non-empty) -> japm.conf global
    -> "" (auto-detect later). Empty instance values never clobber globals.
    """
    cfg = dict(GLOBAL_CFG)
    server = instance_cfg.get("server", {})
    for key in ("ja_path", "engine", "fs_game"):
        if server.get(key):
            cfg[key] = server[key]
        elif key not in cfg:
            cfg[key] = ""
    instance_cfg["server"].update(cfg)
    return instance_cfg


RTVRTM_DEFAULTS = {
    "flood protection": "3", "use say only": "0", "name protection": "1",
    "default game": "", "clean log": "2 10",
    "admin voting": "0 2", "admin minimum votes": "10", "admin skip voting": "1",
    "roundlimit": "0", "timelimit": "0",
    "limit voting": "0 2", "limit minimum votes": "10", "limit extend": "2",
    "limit successful wait time": "300", "limit failed wait time": "300",
    "limit skip voting": "1", "limit second turn": "1", "limit change immediately": "0",
    "rtv": "1", "rtv rate": "50", "rtv voting": "0 3", "rtv minimum votes": "10",
    "rtv extend": "2", "rtv successful wait time": "300", "rtv failed wait time": "300",
    "rtv skip voting": "1", "rtv second turn": "1", "rtv change immediately": "0",
    "automatic maps": "0", "pick secondary maps": "1", "map priority": "2 0 1",
    "nomination type": "0", "enable recently played maps": "1800",
    "rtm": "0", "mode priority": "2 0 2 0 2 1", "rtm rate": "0",
    "rtm voting": "0 3", "rtm minimum votes": "20", "rtm extend": "2",
    "rtm successful wait time": "300", "rtm failed wait time": "300",
    "rtm skip voting": "1", "rtm second turn": "0", "rtm change immediately": "1",
}


def load_config(name):
    path = CONFIG_DIR / ("%s.json" % name)
    if not path.exists():
        print("[ERROR] Config not found: %s" % path)
        sys.exit(1)
    with open(path) as f:
        cfg = json.load(f)
    cfg["name"] = name  # Filename always wins
    return merge_config(cfg)


def pid_path(name, label):
    PID_DIR.mkdir(parents=True, exist_ok=True)
    return PID_DIR / ("%s_%s.pid" % (name, label))


def read_pid(name, label):
    p = pid_path(name, label)
    if p.exists():
        try:
            with open(p) as f:
                return int(f.read().strip())
        except (ValueError, OSError):
            pass
    return None


def write_pid(name, label, pid):
    p = pid_path(name, label)
    with open(p, "w") as f:
        f.write(str(pid))


def remove_pid(name, label):
    p = pid_path(name, label)
    if p.exists():
        p.unlink()


def is_pid_alive(pid):
    if pid is None or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except (OSError, PermissionError):
        return False


def kill_pid(pid, sig=None):
    if pid is None or pid <= 0:
        return False
    if sig is None:
        sig = signal.SIGTERM
    try:
        if IS_WINDOWS:
            import ctypes
            handle = ctypes.windll.kernel32.OpenProcess(1, False, pid)
            if handle:
                ctypes.windll.kernel32.TerminateProcess(handle, 1)
                ctypes.windll.kernel32.CloseHandle(handle)
            return True
        else:
            os.kill(pid, sig)
            return True
    except (OSError, PermissionError, ImportError):
        try:
            subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                           capture_output=True, timeout=5)
            return True
        except Exception:
            return False


RCON_PREFIX = bytes([0xff, 0xff, 0xff, 0xff])


class RCONClient:
    """UDP RCON connection to the game server (stock Q3 protocol)."""

    def __init__(self, password, host="127.0.0.1", port=29070):
        self._addr = (host, port)
        self._password = password
        self._sock = None

    def connect(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(4)

    def disconnect(self):
        if self._sock:
            self._sock.close()
            self._sock = None

    def send(self, command):
        if not self._sock:
            return None
        cmd = RCON_PREFIX + b"rcon " + ("%s %s" % (self._password, command)).encode()
        try:
            self._sock.sendto(cmd, self._addr)
            data = self._sock.recv(4096)
            return data.decode("utf-8", errors="replace").strip()
        except socket.timeout:
            return None
        except OSError:
            return None

    def cvar(self, name, value=None):
        if value is not None:
            return self.send("set %s=%s" % (name, value))
        resp = self.send(name)
        if resp:
            m = re.search(r'"([^"]*)"', resp)
            if m:
                return m.group(1)
        return resp


class LogWatcher:
    """Reads the server log and fires events to the PluginManager."""

    PLAYER_RE = re.compile(r'^(\d+):\s+(.+?)\s+say:\s+"(.+)"')
    TEAM_RE = re.compile(r'^(\d+):\s+(.+?)\s+sayteam:\s+"(.+)"')
    KILL_RE = re.compile(r"Kill:\s+(\d+)\s+(\d+)\s+.+?: (.+?)(?: teamkilled)?$")
    CONNECT_RE = re.compile(r"ClientConnect:\s+(\d+)")
    DISCONNECT_RE = re.compile(r"ClientDisconnect:\s+(\d+)")
    BEGIN_RE = re.compile(r"ClientBegin:\s+(\d+)")
    MAP_RE = re.compile(r"InitGame:\\")
    EXIT_RE = re.compile(r"^Exit:")

    def __init__(self, log_path, plugin_mgr):
        self._path = log_path
        self._pm = plugin_mgr
        self._file = None

    def start(self):
        if not os.path.exists(self._path):
            return
        self._file = open(self._path, "r", encoding="utf-8", errors="replace")
        self._file.seek(0, 2)

    def stop(self):
        if self._file:
            self._file.close()
            self._file = None

    def poll(self):
        """Read new lines from log and dispatch events."""
        if not self._file:
            return
        lines = self._file.read()
        if not lines:
            return
        for raw in lines.split("\n"):
            line = raw.strip()
            if not line:
                continue
            self._parse_line(line)

    def _parse_line(self, line):
        m = self.PLAYER_RE.match(line)
        if m:
            e = events.PlayerChatEvent(m.group(1), m.group(2), m.group(3))
            self._pm.dispatch(e)
            return

        m = self.TEAM_RE.match(line)
        if m:
            e = events.PlayerChatEvent(m.group(1), m.group(2), m.group(3), team=True)
            self._pm.dispatch(e)
            return

        m = self.KILL_RE.match(line)
        if m:
            e = events.PlayerKillEvent(m.group(1), m.group(2), m.group(3))
            self._pm.dispatch(e)
            return

        if line.startswith("ShutdownGame"):
            self._pm.dispatch(events.Event(events.EVENT_SERVER_SHUTDOWN))
        elif line.startswith("ClientConnect:"):
            sp = line.split()
            pid = sp[1] if len(sp) > 1 else "0"
            ip = ""
            for token in sp:
                if token.count(".") >= 2 and ("@" in token or ":" in token):
                    ip = token.strip("@").rsplit(":", 1)[0]
                    break
            e = events.Event(events.EVENT_PLAYER_CONNECT, {"player_id": pid, "ip": ip})
            self._pm.dispatch(e)
        elif line.startswith("ClientDisconnect:"):
            e = events.Event(events.EVENT_PLAYER_DISCONNECT, {"player_id": line.split()[1]})
            self._pm.dispatch(e)
        elif line.startswith("ClientBegin:"):
            e = events.Event(events.EVENT_PLAYER_BEGIN, {"player_id": line.split()[1]})
            self._pm.dispatch(e)
        elif line.startswith("Exit:"):
            self._pm.dispatch(events.Event(events.EVENT_ROUND_EXIT))
        elif self.MAP_RE.match(line):
            map_name = ""
            if "\\mapname\\" in line:
                try:
                    map_name = line.split("\\mapname\\")[1].split("\\")[0]
                except IndexError:
                    pass
            self._pm.dispatch(events.Event(events.EVENT_MAP_CHANGE, {"map": map_name}))


def build_env(cfg=None):
    """Process env for the engine, plus japlusproxy toggles."""
    env = os.environ.copy()
    if not cfg:
        return env
    proxy = cfg.get("proxy", {}) if isinstance(cfg.get("proxy"), dict) else {}
    libdir = game_lib_dir(cfg)
    if libdir is None:
        libdir = ja_gamedata(cfg)  # deploy target unknown yet; preflight decides
    real = proxy.get("real_lib", "") or (
        str(libdir / REAL_LIB_NAME) if libdir is not None else "")
    if proxy.get("enabled", True):
        env["JAPLUS_REAL"] = real
    if proxy.get("no_hooks"):
        env["JAPLUS_NO_HOOKS"] = "1"
    return env


def build_template_values(cfg):
    game = cfg.get("game", {})
    sec = cfg.get("security", {})
    out = {
        "instance_name": cfg["name"],
        "host_name": cfg["server"].get("host_name", "JA+ Server"),
        "message_of_the_day": game.get("message_of_the_day", "").replace("\n", "\\n"),
        "rcon_password": sec.get("rcon_password", ""),
        "server_password": sec.get("server_password", ""),
        "log_name": "%s-games.log" % cfg["name"],
        "starting_map": game.get("starting_map", "mp/ffa5"),
        "gametype": str(game.get("gametype", 0)),
        "maxclients": str(game.get("maxclients", 14)),
        "timelimit": str(game.get("timelimit", 40)),
        "fraglimit": str(game.get("fraglimit", 0)),
        "capturelimit": str(game.get("capturelimit", 20)),
        "duellimit": str(game.get("duellimit", 5)),
        "duel_fraglimit": str(game.get("duel_fraglimit", 1)),
        "council_pass": sec.get("council_password", ""),
        "knight_pass": sec.get("knight_password", ""),
        "instructor_pass": sec.get("instructor_password", ""),
        "clan_pass": game.get("clan_pass", ""),
        "clan_tag": game.get("clan_tag", ""),
    }
    for key, val in game.get("cvars", {}).items():
        out["cvar_%s" % key] = str(val)
    # Raw extra cvars appended verbatim to server.cfg.
    out["extra_cvars"] = "\n".join(
        'seta %s "%s"' % (k, v) for k, v in game.get("cvars", {}).items())
    return out


def find_japlus():
    """Auto-detect JA+ GameData dir (holds linuxjampded + jampgamei386.so).

    Current directory first: just run japm where the server lives and it
    works with no configuration.
    """
    home = Path.home()
    candidates = [Path.cwd()]
    if IS_LINUX:
        candidates += [
            home / "JediAcademy" / "GameData",
            home / "japlus" / "GameData",
            Path("/opt/japlus/GameData"),
            Path("/home/japlus/GameData"),
            home / ".local" / "share" / "openjk",
        ]
    elif IS_WINDOWS:
        candidates += [
            Path("C:/Program Files (x86)/LucasArts/Star Wars Jedi Knight Jedi Academy/GameData"),
            Path("C:/Program Files/LucasArts/Star Wars Jedi Knight Jedi Academy/GameData"),
        ]
    for p in candidates:
        try:
            if (p / ENGINE_BIN).exists() or (p / GAME_LIB).exists():
                return p
        except OSError:
            continue
    return None


def ja_gamedata(cfg):
    """Resolve the GameData dir, or None when nothing is configured/found.

    Order: instance server.ja_path -> JAPLUS_HOME env -> auto-detect.
    Never returns a bare "." -- callers treat None as "not configured".
    """
    path = cfg["server"].get("ja_path", "")
    if not path:
        path = os.environ.get("JAPLUS_HOME", "")
    if not path:
        detected = find_japlus()
        if detected:
            path = str(detected)
            cfg["server"]["ja_path"] = path
    if not path or path == ".":
        return None
    return Path(path)


def ja_moddir(cfg):
    """Writable mod dir (fs_game): <GameData>/japlus. None if unconfigured."""
    gamedata = ja_gamedata(cfg)
    if gamedata is None:
        return None
    fs_game = cfg["server"].get("fs_game", "") or FS_GAME
    return gamedata / fs_game


def game_lib_dirs(cfg):
    """Candidate dirs for the game lib, fs_game first then GameData root."""
    gamedata = ja_gamedata(cfg)
    if gamedata is None:
        return []
    moddir = ja_moddir(cfg)
    dirs = []
    if moddir is not None:
        dirs.append(moddir)
    dirs.append(gamedata)
    return dirs


def game_lib_dir(cfg):
    """Dir actually holding the game lib (wrapper or original). The wrapper
    must be deployed here -- wherever the engine loads it from. None if
    no lib found in any candidate dir.
    """
    for d in game_lib_dirs(cfg):
        try:
            if (d / GAME_LIB).exists() or (d / REAL_LIB_NAME).exists():
                return d
        except OSError:
            continue
    return None


def generate_server_cfg(cfg):
    out_dir = ja_moddir(cfg)
    if out_dir is None:
        print("[ERROR] GameData not set -- cannot write server.cfg")
        return None
    out_dir.mkdir(parents=True, exist_ok=True)
    tpl = CONFIG_DIR / "server.template"
    if not tpl.exists():
        print("[ERROR] server.template not found")
        return None
    with open(tpl) as f:
        template = f.read()
    result = template
    for key, val in build_template_values(cfg).items():
        result = result.replace("[%s]" % key, val)
    out = out_dir / ("%s-server.cfg" % cfg["name"])
    with open(out, "w") as f:
        f.write(result)
    return out


def generate_map_files(cfg):
    out_dir = ja_moddir(cfg)
    if out_dir is None:
        print("[ERROR] GameData not set -- cannot write map files")
        return None
    out_dir.mkdir(parents=True, exist_ok=True)
    name = cfg["name"]
    maps = cfg.get("maps", {})
    primary = out_dir / ("%s-maps.txt" % name)
    with open(primary, "w") as f:
        for m in maps.get("primary", []):
            f.write("%s\n" % m)
    secondary = out_dir / ("%s-secondary_maps.txt" % name)
    with open(secondary, "w") as f:
        for m in maps.get("secondary", []):
            f.write("%s\n" % m)


def generate_rtvrtm_cfg(cfg):
    """Legacy standalone vote-plugin config (MBII-coupled keys kept for
    compat). Only generated when the legacy "rtvrtm" plugin is enabled;
    prefer the native "rtv" plugin instead.
    """
    out_dir = ja_moddir(cfg)
    if out_dir is None:
        print("[ERROR] GameData not set -- cannot write rtvrtm.cfg")
        return None
    out_dir.mkdir(parents=True, exist_ok=True)
    rtv_raw = cfg.get("rtvrtm", {})
    rtv = rtv_raw if isinstance(rtv_raw, dict) else {}

    template_path = CONFIG_DIR / "rtvrtm.template"
    if template_path.exists():
        with open(template_path) as f:
            all_fields = {}
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if ":" in line:
                    key, val = line.split(":", 1)
                    all_fields[key.strip().lower()] = val.strip()
    else:
        all_fields = dict(RTVRTM_DEFAULTS)

    all_fields.update({k: v for k, v in rtv.items() if v is not None})

    lines = [
        "* Generated from %s.json (japm; rtvrtm carried over from vmb2m)\n" % cfg["name"],
        "* DO NOT EDIT\n",
    ]
    lines.append("Log: %s\n" % (out_dir / ("%s-games.log" % cfg["name"])))
    lines.append("MBII folder: %s\n" % str(out_dir))  # compat key, points at japlus dir
    lines.append("Address: 127.0.0.1:%s\n" % cfg["server"]["port"])
    lines.append("Bind: 127.0.0.1\n")
    lines.append("Password: %s\n" % cfg["security"]["rcon_password"])
    lines.append("Maps: %s\n" % (out_dir / ("%s-maps.txt" % cfg["name"])))
    lines.append("Secondary maps: %s\n" % (out_dir / ("%s-secondary_maps.txt" % cfg["name"])))
    for key in RTVRTM_FIELD_MAP:
        if key in all_fields:
            lines.append("%s: %s\n" % (key, all_fields[key]))
    out = out_dir / ("%s-rtvrtm.cfg" % cfg["name"])
    with open(out, "w") as f:
        f.writelines(lines)
    return out


# ---------------------------------------------------------------------------
# japlusproxy management
# ---------------------------------------------------------------------------

def _is_elf32(path):
    try:
        with open(path, "rb") as f:
            magic = f.read(6)
        return magic[:4] == b"\x7fELF" and magic[4:5] == b"\x01"
    except OSError:
        return False


def proxy_preflight(cfg):
    """Check proxy files before launch. Returns (ok, message)."""
    proxy = cfg.get("proxy", {}) if isinstance(cfg.get("proxy"), dict) else {}
    if not proxy.get("enabled", True):
        return True, "proxy disabled in config"
    if not IS_LINUX:
        return False, "japlusproxy is Linux i386 only"
    gamedata = ja_gamedata(cfg)
    if gamedata is None:
        return False, ("GameData not set - put the linuxjampded folder in "
                       "japm.conf (ja_path) or server.ja_path (%s.json), "
                       "or export JAPLUS_HOME, or run from that folder" % cfg["name"])
    src = Path(proxy.get("source", "") or str(REPO_PROXY_SO))
    if not src.exists():
        return False, "japlusproxy.so not found at %s (build it: cd proxy && make)" % src
    if not _is_elf32(src):
        return False, "%s is not a 32-bit ELF" % src
    if not gamedata.exists():
        return False, "GameData dir does not exist: %s" % gamedata
    libdir = game_lib_dir(cfg)
    if libdir is None:
        searched = ", ".join(str(d) for d in game_lib_dirs(cfg))
        return False, "no %s (or %s) in %s" % (GAME_LIB, REAL_LIB_NAME, searched)
    msg = "proxy ready (%s -> %s)" % (src, libdir)
    # A second copy elsewhere can shadow the wrapper (engine search order).
    for other in game_lib_dirs(cfg):
        if other != libdir:
            try:
                a = (other / GAME_LIB).read_bytes() if (other / GAME_LIB).exists() else None
                b = (libdir / GAME_LIB).read_bytes() if (libdir / GAME_LIB).exists() else None
            except OSError:
                continue
            if a is not None and b is not None and a != b:
                msg += ("; WARNING: different %s also in %s "
                        "-- engine may load that one instead" % (GAME_LIB, other))
    return True, msg


def ensure_proxy(cfg):
    """Deploy the wrapper layout alongside the game lib, wherever the
    engine loads it from (fs_game dir first, else GameData root):

    jampgamei386.so  -> japlus_real_i386.so   (original JA+ lib, once)
    japlusproxy.so   -> jampgamei386.so       (wrapper takes the name)
    plus JAPLUS_REAL env pointing at the real lib (belt and suspenders).
    Returns True when the engine will load the wrapper.
    """
    proxy = cfg.get("proxy", {}) if isinstance(cfg.get("proxy"), dict) else {}
    if not proxy.get("enabled", True):
        info("proxy disabled -- engine loads the stock game lib")
        return False
    libdir = game_lib_dir(cfg)
    if libdir is None:
        fail("cannot deploy proxy: no game lib found "
             "(server.ja_path or JAPLUS_HOME)")
        return False
    stock = libdir / GAME_LIB
    real = libdir / REAL_LIB_NAME
    src = Path(proxy.get("source", "") or str(REPO_PROXY_SO))
    current = stock.read_bytes() if stock.exists() else b""
    wrapper = src.read_bytes()
    if not real.exists() and stock.exists():
        # First deploy: is the current file already our wrapper?
        if current != wrapper:
            stock.rename(real)
            ok("preserved original %s -> %s" % (GAME_LIB, REAL_LIB_NAME))
        else:
            warn("wrapper already in place but no %s backup; continuing" % REAL_LIB_NAME)
    if current != wrapper:
        stock.write_bytes(wrapper)
        ok("deployed japlusproxy.so as %s" % GAME_LIB)
    else:
        ok("wrapper already deployed as %s" % GAME_LIB)
    if proxy.get("no_hooks"):
        warn("JAPLUS_NO_HOOKS=1 -- inline hooks off, filters stay on")
    return True


def verify_proxy(cfg, log_path, timeout=25):
    """Tail the fresh server log for [japlus_proxy] lines. Returns True if
    at least one hook armed; reports SKIPPED (wrong-build) lines."""
    proxy = cfg.get("proxy", {}) if isinstance(cfg.get("proxy"), dict) else {}
    if not proxy.get("enabled", True):
        return True
    timeout = int(proxy.get("verify_timeout", timeout))
    hooked, skipped = [], []
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(log_path, encoding="utf-8", errors="replace") as f:
                data = f.read()
        except OSError:
            data = ""
        for line in data.splitlines():
            if "[japlus_proxy] hooked" in line and line not in hooked:
                hooked.append(line)
            elif "SKIPPED" in line and line not in skipped:
                skipped.append(line)
        if hooked or skipped:
            break
        time.sleep(1)
    for line in hooked:
        ok(line.strip())
    for line in skipped:
        warn(line.strip())
    if not hooked and not skipped:
        warn("no [japlus_proxy] lines in log -- wrapper may not have loaded; "
             "check that %s is the proxy build" % GAME_LIB)
        return False
    if skipped and not hooked:
        warn("all hooks SKIPPED -- wrong JA+ build for this proxy?")
        return False
    return True


def _engine_alive(name):
    """Check if engine supervisor (or engine) is running."""
    if IS_WINDOWS:
        pid = read_pid(name, "engine")
        return is_pid_alive(pid) if pid else False
    pid = read_pid(name, "engine")
    if is_pid_alive(pid):
        return True  # Supervisor PID is alive
    try:
        r = subprocess.run(["screen", "-list"], capture_output=True, timeout=5, text=True)
        return "jap_%s" % name in r.stdout
    except Exception:
        return False


def _engine_exists(name):
    """Check if a screen session exists with a live engine inside."""
    try:
        r = subprocess.run(["screen", "-list"], capture_output=True, timeout=5, text=True)
        if "jap_%s" % name not in r.stdout:
            return False
        r2 = subprocess.run(["pgrep", "-f", "%s.*%s" % (ENGINE_BIN, name)],
                            capture_output=True, timeout=5)
        return r2.returncode == 0
    except Exception:
        return False


def _engine_kill(name, port=None):
    """Kill engine: screen session, then wait for port to free."""
    if IS_WINDOWS:
        return
    try:
        subprocess.run(["screen", "-S", "jap_%s" % name, "-X", "quit"],
                       capture_output=True, timeout=5)
    except Exception:
        pass
    try:
        subprocess.run(["pkill", "-9", "-f", "screen.*jap_%s" % name],
                       capture_output=True, timeout=5)
    except Exception:
        pass
    try:
        subprocess.run(["screen", "-wipe"], capture_output=True, timeout=5)
    except Exception:
        pass
    try:
        subprocess.run(["pkill", "-9", "-f", "\\+exec %s-server\\.cfg" % name],
                       capture_output=True, timeout=5)
    except Exception:
        pass
    if port:
        for _ in range(15):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.settimeout(1)
                s.connect(("127.0.0.1", port))
                s.send(b"\xff\xff\xff\xffgetstatus")
                s.recv(4096)
                s.close()
                time.sleep(1)
            except Exception:
                return  # port is free
        warn("[%s] Port %d still in use after 15s, will retry" % (name, port))


def find_engine(cfg=None):
    """Auto-detect engine binary. Priority: PATH -> GameData dir."""
    for p in os.environ.get("PATH", "").split(os.pathsep):
        full = Path(p) / ENGINE_BIN
        if full.exists():
            return str(full)

    if cfg:
        gamedata = ja_gamedata(cfg)
        if gamedata is not None:
            full = gamedata / ENGINE_BIN
            if full.exists():
                return str(full)

    return ENGINE_BIN


def _plugin_alive(name):
    """Check if a standalone plugin process is running for this instance."""
    try:
        r = subprocess.run(
            ["pgrep", "-f", "plugins.*%s" % name],
            capture_output=True, timeout=5
        )
        return r.returncode == 0
    except Exception:
        return False


def start_engine(cfg):
    engine = cfg["server"].get("engine", "")
    if not engine:
        engine = find_engine(cfg)
        cfg["server"]["engine"] = engine
    port = cfg["server"]["port"]
    fs_game = cfg["server"].get("fs_game", "") or FS_GAME
    server_cfg = "%s-server.cfg" % cfg["name"]
    gamedata = ja_gamedata(cfg)
    screen_name = "jap_%s" % cfg["name"]
    using_screen = False

    cmd = [
        engine,
        "+set", "dedicated", "2",
        "+set", "net_port", str(port),
        "+set", "fs_homepath", str(gamedata),
        "+set", "fs_game", fs_game,
        "+exec", server_cfg,
    ]

    # Always clean up any stale/dead screen sessions before starting
    if not IS_WINDOWS:
        _engine_kill(cfg["name"], port)
        for _ in range(5):
            r = subprocess.run(["screen", "-list"], capture_output=True, timeout=5, text=True)
            if "jap_%s" % cfg["name"] not in r.stdout:
                break
            subprocess.run(["screen", "-S", "jap_%s" % cfg["name"], "-X", "quit"],
                           capture_output=True, timeout=5)
            subprocess.run(["pkill", "-9", "-f", "jap_%s" % cfg["name"]],
                           capture_output=True, timeout=5)
            subprocess.run(["screen", "-wipe"], capture_output=True, timeout=5)
            time.sleep(1)

    env = build_env(cfg)

    if IS_WINDOWS:
        kwargs = {"cwd": str(gamedata), "env": env,
                  "creationflags": subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP}
    else:
        import shutil as _su
        if _su.which("screen"):
            using_screen = True
            cmd = ["screen", "-dmS", screen_name] + cmd
        kwargs = {"cwd": str(gamedata), "env": env}

    print("  Engine: %s" % " ".join(cmd))

    if using_screen:
        subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env, cwd=str(gamedata))
    else:
        proc = subprocess.Popen(cmd, stderr=subprocess.DEVNULL, **kwargs)
        write_pid(cfg["name"], "engine", proc.pid)
        ok("Engine started (PID %d)" % proc.pid)
        return proc

    # Fork a dedicated supervisor for screen mode
    spid = os.fork()
    if spid == 0:
        # Child: engine supervisor -- only job is to keep engine running
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
        crashes = 0
        while crashes < 10:
            time.sleep(3)
            if _engine_alive(cfg["name"]):
                continue
            if not _engine_exists(cfg["name"]):
                break  # screen session gone, give up
            crashes += 1
            print("[%s] Engine supervisor restarting (crash %d/10)" % (cfg["name"], crashes))
            subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, env=env, cwd=str(gamedata))
            time.sleep(5)
        os._exit(0)
    # Parent: return to main loop
    write_pid(cfg["name"], "engine", spid)
    ok("Engine started in screen: %s (supervisor PID %d)" % (screen_name, spid))
    return None


def start_standalone_plugins(cfg):
    """Spawn standalone plugins (plugins/<name>/<name>.py)."""
    procs = {}
    for pname, settings in cfg.get("plugins", {}).items():
        if isinstance(settings, bool) and not settings:
            continue
        if isinstance(settings, dict) and not settings.get("enabled", True):
            continue
        script = BASE / "plugins" / pname / ("%s.py" % pname)
        if not script.exists():
            continue
        print("  [%s] Starting..." % pname)
        cmd = [sys.executable, str(script)]
        rtvcfg = ja_moddir(cfg) / ("%s-rtvrtm.cfg" % cfg["name"])
        if pname == "rtvrtm" and rtvcfg.exists():
            cmd += ["-c", str(rtvcfg)]
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        write_pid(cfg["name"], pname, proc.pid)
        print("  [%s] Started (PID %d)" % (pname, proc.pid))
        procs[pname] = proc
    return procs


def stop_processes(name, port=None):
    for pid_file in PID_DIR.glob("%s_*.pid" % name):
        label = pid_file.stem.replace(name + "_", "")
        pid = read_pid(name, label)
        if label == "engine" and (pid == 1 or is_pid_alive(pid)):
            # Kill engine: screen session, supervisor, and everything
            _engine_kill(name, port)
            if pid != 1:
                kill_pid(pid)  # Kill supervisor too
            remove_pid(name, label)
            print("  Stopped engine")
        elif pid and is_pid_alive(pid):
            kill_pid(pid)
            print("  Stopped %s (PID %d)" % (label, pid))
            remove_pid(name, label)
        else:
            print("  %s not running" % label)
    time.sleep(1)


def _init_plugins(cfg, rcon_client):
    """Load native plugins (no standalone script found)."""
    pm = PluginManager()
    pm.set_rcon(rcon_client.send)

    for name, settings in cfg.get("plugins", {}).items():
        if isinstance(settings, bool) and not settings:
            continue
        if isinstance(settings, dict) and not settings.get("enabled", True):
            continue
        script = BASE / "plugins" / name / ("%s.py" % name)
        if script.exists():
            continue  # standalone, handled elsewhere
        config = settings if isinstance(settings, dict) else {}
        config["_rcon_password"] = cfg["security"]["rcon_password"]
        config["_rcon_port"] = cfg["server"]["port"]
        if name == "rtv":
            maps = cfg.get("maps", {})
            config["_maps"] = list(maps.get("primary", [])) + list(maps.get("secondary", []))
            config["_gametype"] = cfg.get("game", {}).get("gametype", 0)
        pm.load_from_config({name: config})

    return pm


def cmd_start(name):
    cfg = load_config(name)
    gamedata = ja_gamedata(cfg)
    if gamedata is None or not gamedata.exists():
        fail("GameData not found. Set ja_path in japm.conf (shared) or "
             "server.ja_path in configs/%s.json, or export JAPLUS_HOME, "
             "or run from the folder holding %s." % (name, ENGINE_BIN))
        return
    info("[%s] GameData: %s" % (name, gamedata))
    info("[%s] Generating configs..." % name)
    generate_server_cfg(cfg)
    generate_map_files(cfg)
    # Legacy MBII-coupled standalone config: only for the carried-over
    # rtvrtm plugin, which is off by default (use native "rtv" instead).
    legacy = cfg.get("plugins", {}).get("rtvrtm", False)
    if (isinstance(legacy, dict) and legacy.get("enabled", True)) or legacy is True:
        generate_rtvrtm_cfg(cfg)

    pid = read_pid(name, "engine")
    if pid and pid != 1 and is_pid_alive(pid):
        warn("[%s] Engine already running (PID %d)" % (name, pid))
        return
    if pid == 1 and _engine_alive(name):
        warn("[%s] Engine already running (screen)" % name)
        return

    # japlusproxy preflight + deploy before the engine loads the game lib
    proxy_ok, proxy_msg = proxy_preflight(cfg)
    if proxy_ok:
        info("[%s] Proxy: %s" % (name, proxy_msg))
        ensure_proxy(cfg)
    else:
        fail("[%s] Proxy: %s" % (name, proxy_msg))
        if (cfg.get("proxy", {}) or {}).get("enabled", True):
            fail("[%s] refusing to start with proxy enabled but broken "
                 "(set proxy.enabled=false to bypass)" % name)
            return

    info("[%s] Launching..." % name)
    engine = start_engine(cfg)

    info("[%s] Waiting for engine..." % name)
    for _ in range(15):
        if _engine_alive(name):
            break
        time.sleep(1)

    # RCON connection for native plugins + log watching
    rcon = RCONClient(cfg["security"]["rcon_password"], port=cfg["server"]["port"])
    rcon.connect()
    pm = _init_plugins(cfg, rcon)

    # Log watcher feeds events to plugins (log lives under fs_game dir)
    log_path = ja_moddir(cfg) / ("%s-games.log" % name)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    if not log_path.exists():
        log_path.touch()
    else:
        log_path.write_text("")  # Clear old log on start/restart
    watcher = LogWatcher(str(log_path), pm)
    watcher.start()

    # Confirm the wrapper actually loaded and hooks armed
    verify_proxy(cfg, log_path)

    standalone = start_standalone_plugins(cfg)

    # Single fork (daemonize)
    if not IS_WINDOWS and not os.environ.get("JAPM_FG"):
        pid = os.fork()
        if pid > 0:
            write_pid(name, "manager", pid)
            print("  Manager running as daemon (PID %d)" % pid)
            return
        # Detach from terminal so child survives logout
        os.setsid()
        sys.stdin = open(os.devnull)
        sys.stdout = open(os.devnull, "w")
        sys.stderr = open(os.devnull, "w")

    pm.start_all()

    print("[%s] Watching processes (auto-restart enabled)..." % name)
    if cfg.get("server", {}).get("restart_every_hours"):
        print("[%s] Scheduled restart every %d hours" % (name, cfg["server"]["restart_every_hours"]))

    if not IS_WINDOWS:
        signal.signal(signal.SIGHUP, lambda s, f: None)

    crashes = 0
    max_crashes = 5
    tick = 0
    _restarting = False
    engine_start = time.time()
    restart_hours = cfg.get("server", {}).get("restart_every_hours", 0)

    try:
        while True:
            try:
                tick += 1
                watcher.poll()
                pm.loop_all()

                try:
                    engine_alive = is_pid_alive(engine.pid) if engine and hasattr(engine, 'pid') else _engine_alive(name)
                except Exception:
                    engine_alive = False  # Assume dead on check failure

                for sname, sproc in list(standalone.items()):
                    if is_pid_alive(sproc.pid):
                        continue
                    sproc.poll()
                    print("  [%s] died, restarting..." % sname)
                    script = BASE / "plugins" / sname / ("%s.py" % sname)
                    cmd = [sys.executable, str(script)]
                    rtvcfg = ja_moddir(cfg) / ("%s-rtvrtm.cfg" % cfg["name"])
                    if sname == "rtvrtm" and rtvcfg.exists():
                        cmd += ["-c", str(rtvcfg)]
                    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    write_pid(name, sname, p.pid)
                    standalone[sname] = p  # Update tracking so it's not respawned

                # Scheduled restart -- spawn restart (stop then start)
                if engine_alive and restart_hours > 0:
                    elapsed = time.time() - engine_start
                    if elapsed >= restart_hours * 3600:
                        info("[%s] Scheduled restart after %d hours" % (name, restart_hours))
                        _restarting = True
                        subprocess.Popen(
                            [sys.executable, sys.argv[0], name, "restart"],
                            stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL,
                        )
                        break

                if not engine_alive:
                    if engine:
                        engine.poll()
                        code = engine.returncode if engine.returncode is not None else -1
                        is_scheduled = code == -1
                    else:
                        code = -1
                        is_scheduled = True
                    if is_scheduled:
                        crashes = 0
                        print("[%s] Performing scheduled restart..." % name)
                    else:
                        crashes += 1
                        fail("[%s] Engine crashed (exit %d, crash %d/%d)" % (
                            name, code, crashes, max_crashes))
                        if crashes >= max_crashes:
                            print("[%s] Max crashes reached, giving up" % name)
                            break
                    print("[%s] Restarting engine in 5s..." % name)
                    time.sleep(5)
                    ok, msg = proxy_preflight(cfg)
                    if ok:
                        ensure_proxy(cfg)
                    engine = start_engine(cfg)
                    engine_start = time.time()
                    time.sleep(3)
                    if not engine and _engine_alive(name):
                        ok("[%s] Engine restarted successfully" % name)
                    elif not engine:
                        fail("[%s] Engine failed to start" % name)
                    verify_proxy(cfg, log_path)
                    new_standalone = start_standalone_plugins(cfg)
                    standalone.update(new_standalone)
            except Exception as e:
                fail("[%s] Watchdog error: %s" % (name, e))
                import traceback
                traceback.print_exc()
            time.sleep(2)
    except KeyboardInterrupt:
        print("\n%s[%s] Shutting down...%s" % (C.YELLOW, name, C.END))
    finally:
        pm.finish_all()
        watcher.stop()
        rcon.disconnect()
        if not _restarting:
            stop_processes(name, cfg["server"]["port"])
            print("[%s] Stopped" % name)
        else:
            print("[%s] Old daemon exiting, new process taking over" % name)


def cmd_stop(name):
    print("[%s] Stopping..." % name)
    try:
        cfg = load_config(name)
        stop_processes(name, cfg["server"]["port"])
    except Exception:
        stop_processes(name)
    print("[%s] Stopped" % name)


def cmd_proxy(name):
    """Check proxy deployment + preflight without starting the server."""
    cfg = load_config(name)
    gamedata = ja_gamedata(cfg)
    print("Instance: %s" % name)
    print("  GameData: %s" % (str(gamedata) if gamedata is not None else
                              "(not set -- japm.conf ja_path, server.ja_path, or JAPLUS_HOME)"))
    print("  Engine:   %s" % (cfg["server"].get("engine") or find_engine(cfg)))
    print("  fs_game:  %s" % (cfg["server"].get("fs_game") or FS_GAME))
    p_ok, p_msg = proxy_preflight(cfg)
    print("  Proxy:    %s %s" % ("OK" if p_ok else "BROKEN", p_msg))
    libdir = game_lib_dir(cfg)
    if libdir is not None:
        real = libdir / REAL_LIB_NAME
        print("  Game lib dir: %s" % libdir)
        print("  Wrapper deployed: %s" % ("yes" if real.exists() else "no (will deploy on start)"))
        print("  JAPLUS_REAL -> %s" % (cfg.get("proxy", {}) or {}).get("real_lib", str(real)))


def cmd_restart(name):
    cmd_stop(name)
    time.sleep(2)
    cmd_start(name)


def cmd_status(name):
    cfg = load_config(name)
    print("Instance: %s" % name)
    print("  Port:    %d" % cfg["server"]["port"])
    print("  Engine:  %s" % (cfg["server"].get("engine") or "(auto)"))
    print("  GameData:%s" % (cfg["server"].get("ja_path") or "(auto)"))
    seen = set()
    for pid_file in sorted(PID_DIR.glob("%s_*.pid" % name)):
        label = pid_file.stem.replace(name + "_", "")
        seen.add(label)
        pid = read_pid(name, label)
        if label == "engine" and pid == 1:
            alive = _engine_alive(name)
            status = "%sRUNNING%s" % (C.GREEN, C.END) if alive else "%sSTOPPED%s" % (C.RED, C.END)
            print("  engine: %s (screen)" % status)
        else:
            alive = is_pid_alive(pid)
            status = "%sRUNNING%s" % (C.GREEN, C.END) if alive else "%sSTOPPED%s" % (C.RED, C.END)
            print("  %s: %s (PID %s)" % (label, status, str(pid) if pid else "-"))
    if not seen:
        print("  (nothing running)")


def cmd_list():
    print("Instances:")
    for f in sorted(CONFIG_DIR.glob("*.json")):
        name = f.stem
        pid = read_pid(name, "engine")
        alive = is_pid_alive(pid) or (pid == 1 and _engine_alive(name))
        marker = "%sRUNNING%s" % (C.GREEN, C.END) if alive else "%sSTOPPED%s" % (C.RED, C.END)
        print("  %s [%s]" % (name, marker))


def main():
    if len(sys.argv) < 2:
        print("Usage: japm.py <name> <start|stop|restart|status|proxy>")
        print("       japm.py --list")
        sys.exit(1)

    if sys.argv[1] == "--list":
        cmd_list()
        return

    name = sys.argv[1]
    action = sys.argv[2] if len(sys.argv) > 2 else "start"

    actions = {
        "start": cmd_start,
        "stop": cmd_stop,
        "restart": cmd_restart,
        "status": cmd_status,
        "proxy": cmd_proxy,
    }
    fn = actions.get(action)
    if fn:
        fn(name)
    else:
        print("Unknown action: %s" % action)
        sys.exit(1)


if __name__ == "__main__":
    main()
