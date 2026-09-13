"""Rock-the-vote for basejka JA+ servers (native japm plugin).

Players nominate maps (!nominate <map>) and rock the vote (!rtv). Past the
rate threshold a numbered vote runs (!1..!N); the winner switches via RCON
(`g_gametype` + `map`). No MBII/mbmode dependency.

Basejka gametypes: 0 FFA, 1 Holocron, 2 JediMaster, 3 Duel, 4 PowerDuel,
6 Team, 7 Siege, 8 CTF.
"""

import random
import time

from plugins.base import BasePlugin
from plugins import event_types as events

GAMETYPES = {0, 1, 2, 3, 4, 6, 7, 8}


class RTVPlugin(BasePlugin):
    name = "rtv"
    version = "1.0"

    def on_init(self, api, config):
        self.api = api
        self.cfg = config
        self.allow = bool(config.get("allow", True))
        self.rate = float(config.get("rate", 60))
        self.min_players = int(config.get("min_players", 0))
        self.cooldown_ok = int(config.get("cooldown_success", 300))
        self.cooldown_no = int(config.get("cooldown_fail", 120))
        self.vote_time = int(config.get("vote_time", 30))
        self.change_now = bool(int(config.get("change_immediately", 1)))
        self.noms_per_player = int(config.get("nominations_per_player", 1))
        self.max_noms = int(config.get("max_nominations", 5))
        self.recent_secs = int(config.get("recently_played", 1800))
        self.allow_extend = bool(int(config.get("allow_extend", 1)))
        self.map_gametypes = config.get("map_gametypes", {}) or {}
        self.maps = list(config.get("_maps", []) or [])
        self.gametype = config.get("_gametype", 0)
        try:
            self.gametype = int(self.gametype)
        except (TypeError, ValueError):
            self.gametype = 0
        if self.gametype not in GAMETYPES:
            self.gametype = 0

        self.players = {}      # pid -> name (BEGIN-tracked, voting population)
        self.rtv = {}          # pid -> True (current rockers)
        self.noms = {}         # map -> pid (who nominated)
        self.recent = []       # [(map, timestamp)] recently played/won
        self.vote = None       # active vote dict or None
        self.cool_until = 0
        self.pending_map = None  # won but deferred (change_immediately=0)
        self.current_map = ""

        if not self.allow or not self.maps:
            print("  [rtv] disabled (allow=false or empty map list)")
            return False
        print("  [rtv] Active — !rtv @%g%%, %d maps, vote %ds"
              % (self.rate, len(self.maps), self.vote_time))
        return True

    # -- helpers ---------------------------------------------------------

    def _now(self):
        return time.time()

    def _say(self, msg):
        try:
            self.api.say(msg)
        except Exception:
            pass

    def _threshold(self):
        n = len(self.players)
        if n <= 0:
            return 1
        need = self.rate / 100.0 * n
        need = max(2 if n >= 2 else 1, need)
        return int(need) if need == int(need) else int(need) + 1

    def _prune_recent(self):
        if self.recent_secs <= 0:
            return
        cutoff = self._now() - self.recent_secs
        self.recent = [(m, t) for m, t in self.recent if t > cutoff]

    def _is_recent(self, m):
        self._prune_recent()
        return any(m == r for r, _ in self.recent)

    def _valid_map(self, m):
        return m in self.maps

    def _map_gametype(self, m):
        try:
            g = int(self.map_gametypes.get(m, self.gametype))
        except (TypeError, ValueError):
            g = self.gametype
        return g if g in GAMETYPES else self.gametype

    def _rcon(self, cmd):
        try:
            return self.api.rcon(cmd)
        except Exception:
            return None

    def _switch(self, m):
        g = self._map_gametype(m)
        if g != self.gametype:
            self._rcon("set g_gametype %d" % g)
            self.gametype = g
        self._rcon("map %s" % m)
        self.recent.append((m, self._now()))
        self.current_map = m

    def _eligible(self, pid):
        if pid not in self.players:
            return False
        if len(self.players) < self.min_players:
            return False
        return True

    # -- vote lifecycle --------------------------------------------------

    def _options(self):
        opts = [m for m in self.noms if self._valid_map(m) and not self._is_recent(m)]
        random.shuffle(opts)
        opts = opts[:self.max_noms]
        if len(opts) < self.max_noms:
            fill = [m for m in self.maps
                    if m not in opts and not self._is_recent(m) and m != self.current_map]
            random.shuffle(fill)
            opts += fill[:self.max_noms - len(opts)]
        if self.allow_extend:
            opts.append("Extend current map")
        return opts[:max(1, self.max_noms + (1 if self.allow_extend else 0))]

    def _start_vote(self):
        opts = self._options()
        if not opts:
            self._say("RTV: no eligible maps right now.")
            self.rtv = {}
            return
        self.vote = {"options": opts, "votes": {}, "ends": self._now() + self.vote_time}
        names = "  ".join("%d.%s" % (i + 1, o) for i, o in enumerate(opts))
        self._say("RTV: vote for next map! Type !1-!%d (%ds): %s"
                  % (len(opts), self.vote_time, names))

    def _tally(self, votes):
        counts = {}
        for pid, idx in votes.items():
            if pid in self.players:
                counts[idx] = counts.get(idx, 0) + 1
        if not counts:
            return None
        top = max(counts.values())
        winners = [i for i, c in counts.items() if c == top]
        return random.choice(winners)

    def _finish_vote(self, timed_out):
        v = self.vote
        self.vote = None
        if v is None:
            return
        won = self._tally(v["votes"])
        if won is None:
            self._say("RTV failed: nobody voted.")
            self.cool_until = self._now() + self.cooldown_no
            self.rtv = {}
            self.noms = {}
            return
        pick = v["options"][won]
        if pick == "Extend current map":
            self._say("RTV: extending current map.")
            self.cool_until = self._now() + self.cooldown_ok
        elif self.change_now:
            self._say("RTV passed: changing to %s." % pick)
            self._switch(pick)
            self.cool_until = self._now() + self.cooldown_ok
        else:
            self._say("RTV passed: %s will load next." % pick)
            self.pending_map = pick
            self.cool_until = self._now() + self.cooldown_ok
        self.rtv = {}
        self.noms = {}

    def _cast(self, pid, idx):
        v = self.vote
        if v is None or not (0 <= idx < len(v["options"])):
            return
        if pid not in self.players:
            return
        v["votes"][pid] = idx
        if len(v["votes"]) >= len(self.players) and self.players:
            self._finish_vote(False)

    # -- commands --------------------------------------------------------

    def _cmd_rtv(self, pid):
        if not self._eligible(pid):
            return
        now = self._now()
        if now < self.cool_until:
            self._say("%s: vote cooldown (%ds left)."
                      % (self.players.get(pid, pid), int(self.cool_until - now)))
            return
        if self.vote is not None:
            return
        self.rtv[pid] = True
        need = self._threshold()
        if len(self.rtv) >= need:
            self._start_vote()
        else:
            self._say("%s rocked the vote (%d/%d)."
                      % (self.players.get(pid, pid), len(self.rtv), need))

    def _cmd_unrtv(self, pid):
        if pid in self.rtv:
            del self.rtv[pid]
            self._say("%s withdrew (%d/%d)."
                      % (self.players.get(pid, pid), len(self.rtv), self._threshold()))

    def _cmd_nominate(self, pid, arg):
        if not self._eligible(pid):
            return
        m = (arg or "").strip()
        if not m:
            self._say("%s: usage: !nominate <map>." % self.players.get(pid, pid))
            return
        if not self._valid_map(m):
            self._say("%s: unknown map '%s'." % (self.players.get(pid, pid), m))
            return
        if self._is_recent(m):
            self._say("%s: %s was played recently." % (self.players.get(pid, pid), m))
            return
        mine = [k for k, v in self.noms.items() if v == pid]
        if len(mine) >= self.noms_per_player and m not in self.noms:
            self._say("%s: nomination limit reached." % self.players.get(pid, pid))
            return
        self.noms[m] = pid
        self._say("%s nominated %s." % (self.players.get(pid, pid), m))

    def _handle_text(self, pid, text):
        t = (text or "").strip()
        low = t.lower()
        if self.vote is not None and (t.isdigit() or (low.startswith("!") and low[1:].isdigit())):
            num = t if t.isdigit() else low[1:]
            self._cast(pid, int(num) - 1)
            return True
        word = low[1:] if low.startswith("!") else low
        if word == "rtv":
            self._cmd_rtv(pid)
            return True
        if word == "unrtv":
            self._cmd_unrtv(pid)
            return True
        if word == "nominate" or word.startswith("nominate "):
            self._cmd_nominate(pid, word[8:].strip())
            return True
        return False

    # -- events ----------------------------------------------------------

    def on_event(self, event):
        et = event.type
        if et == events.EVENT_PLAYER_BEGIN:
            pid = str(event.data.get("player_id", ""))
            if pid:
                self.players.setdefault(pid, "")
            return False
        if et == events.EVENT_PLAYER_CONNECT:
            return False
        if et == events.EVENT_PLAYER_DISCONNECT:
            pid = str(event.data.get("player_id", ""))
            self.players.pop(pid, None)
            self.rtv.pop(pid, None)
            if self.vote is not None and pid in self.vote["votes"]:
                del self.vote["votes"][pid]
            return False
        if et in (events.EVENT_PLAYER_CHAT, events.EVENT_PLAYER_COMMAND):
            pid = str(event.data.get("player_id", ""))
            name = event.data.get("player_name", "")
            if pid and name:
                self.players[pid] = name
            if et == events.EVENT_PLAYER_COMMAND:
                cmd = event.data.get("command", "")
                args = event.data.get("args", "")
                if cmd in ("rtv", "unrtv"):
                    self._handle_text(pid, "!" + cmd)
                    return True
                if cmd == "nominate":
                    self._cmd_nominate(pid, args)
                    return True
                if cmd.isdigit():
                    self._handle_text(pid, cmd)
                    return True
                return False
            return self._handle_text(pid, event.data.get("message", ""))
        if et == events.EVENT_MAP_CHANGE:
            m = event.data.get("map", "")
            if m:
                if self.pending_map and m != self.pending_map:
                    self._switch(self.pending_map)
                else:
                    self.current_map = m
                    self.recent.append((m, self._now()))
                self.pending_map = None
            self.rtv = {}
            self.noms = {}
            self.vote = None
            return False
        return False

    def on_loop(self):
        if self.vote is not None and self._now() >= self.vote["ends"]:
            self._finish_vote(True)
