"""VPN/proxy detection plugin using iphub.info API."""

import ipaddress
import json
import urllib.request
import urllib.error

from plugins.base import BasePlugin
from plugins import event_types as events

API_URL = "http://v2.api.iphub.info/ip/%s"


class VPNMonitorPlugin(BasePlugin):
    name = "vpnmonitor"
    version = "1.0"

    def on_init(self, api, config):
        self.api = api
        self.apikey = config.get("apikey", "")
        self.block = config.get("block", [1, 2])
        self.action = config.get("action", 0)
        self.announce = config.get("announce", True)
        self._cache = {}

        if not self.apikey:
            print("  [vpnmonitor] No API key set — disabled")
            return False

        self._parse_list("whitelist", config)
        self._parse_list("blacklist", config)

        print("  [vpnmonitor] Active — monitoring connections")
        return True

    def _parse_list(self, key, config):
        raw = config.get(key, [])
        result = []
        for e in raw:
            try:
                if isinstance(e, list) and len(e) == 2:
                    result.append(("range", ipaddress.ip_address(e[0]), ipaddress.ip_address(e[1])))
                else:
                    result.append(("single", ipaddress.ip_address(str(e))))
            except ValueError:
                pass
        setattr(self, key, result)

    def _in_list(self, ip, lst):
        try:
            target = ipaddress.ip_address(ip)
        except ValueError:
            return False
        for entry in lst:
            if entry[0] == "single" and target == entry[1]:
                return True
            if entry[0] == "range" and entry[1] <= target <= entry[2]:
                return True
        return False

    def _check_ip(self, ip):
        if ip in self._cache:
            return self._cache[ip]

        req = urllib.request.Request(API_URL % ip, headers={"X-Key": self.apikey})
        try:
            resp = urllib.request.urlopen(req, timeout=5)
            data = json.loads(resp.read())
            vpn = data.get("block", 0)
            self._cache[ip] = vpn
            return vpn
        except (urllib.error.URLError, json.JSONDecodeError, OSError) as e:
            print("  [vpnmonitor] API error for %s: %s" % (ip, e))
            self._cache[ip] = -1
            return -1

    def on_event(self, event):
        if event.type != events.EVENT_PLAYER_CONNECT:
            return False

        ip = event.data.get("ip", "")
        pid = event.data.get("player_id", "")
        if not ip or ip == "127.0.0.1":
            return False

        if self._in_list(ip, self.whitelist):
            return False

        if self._in_list(ip, self.blacklist):
            if self.action == 1:
                self.api.ban(ip)
            self.api.kick(pid)
            if self.announce:
                self.api.say("^9[VPN]^7 Kicked player for suspected VPN usage")
            return True

        vpn = self._check_ip(ip)
        if vpn in self.block:
            if self.action == 1:
                self.api.ban(ip)
            self.api.kick(pid)
            if self.announce:
                self.api.say("^9[VPN]^7 Kicked player for suspected VPN usage")
            return True

        return False
