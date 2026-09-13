"""Rotates messages in chat every N seconds. Uses own thread (godfinger pattern)."""

import logging
import os
import socket
import threading
import time

from plugins.base import BasePlugin

LOG = logging.getLogger("automessage")


class AutoMessagePlugin(BasePlugin):
    name = "automessage"
    version = "1.0"

    def on_init(self, api, config):
        self._stop = False
        self._messages = config.get("messages", [])
        self._interval = config.get("interval", 300)
        self._prefix = bytes([0xff, 0xff, 0xff, 0xff]) + b"rcon "
        self._password = config.get("_rcon_password", "")
        self._port = config.get("_rcon_port", 29070)
        if not self._messages or not self._password:
            return False
        self._index = 0
        print("  [automessage] Active — %d messages every %ds" % (len(self._messages), self._interval))
        return True

    def on_start(self):
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return True

    def _send(self, message):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(2)
            cmd = self._prefix + ("%s svsay %s" % (self._password, message)).encode()
            s.connect(("127.0.0.1", self._port))
            s.send(cmd)
            s.recv(4096)
            s.close()
        except Exception:
            pass

    def _run(self):
        time.sleep(5)
        while not self._stop:
            msg = self._messages[self._index % len(self._messages)]
            self._index += 1
            self._send("%s" % msg)
            time.sleep(self._interval)

    def on_finish(self):
        self._stop = True
