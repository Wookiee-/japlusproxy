"""Loads and manages native plugins from plugins/<name>/__init__.py"""

import importlib
import logging
import os
import traceback

from .base import BasePlugin
from . import event_types as events

PLUGIN_DIR = os.path.dirname(os.path.abspath(__file__))
LOG = logging.getLogger("plugin_mgr")


class PluginAPI:
    """Thin API given to every plugin — wraps RCON connection."""

    def __init__(self, rcon_callback):
        self._rcon = rcon_callback

    def say(self, message):
        return self._rcon("svsay %s" % message)

    def csay(self, message):
        return self._rcon("say %s" % message)

    def rcon(self, command):
        return self._rcon(command)

    def tell(self, pid, message):
        return self._rcon("svtell %s %s" % (pid, message))

    def kick(self, pid):
        return self._rcon("clientkick %s" % pid)

    def ban(self, ip):
        return self._rcon("addip %s" % ip)

    def unban(self, ip):
        return self._rcon("removeip %s" % ip)

    def cvar(self, name, value=None):
        if value is None:
            return self._rcon(name)
        return self._rcon("set %s=%s" % (name, value))

    def map(self, map_name):
        return self._rcon("map %s" % map_name)


class PluginManager:
    def __init__(self):
        self.plugins = []
        self.api = PluginAPI(self._rcon)
        self._rcon_func = None

    def set_rcon(self, func):
        self._rcon_func = func
        self.api = PluginAPI(func)

    def _rcon(self, cmd):
        if self._rcon_func:
            return self._rcon_func(cmd)
        return None

    def load_from_config(self, instances_cfg):
        """instances_cfg: dict of {plugin_name: plugin_settings}
           Only loads plugins where settings.get("type") in (None, "native", "plugin")
           and settings.get("enabled", True) is truthy.
        """
        if not instances_cfg:
            return

        for name, settings in instances_cfg.items():
            ptype = settings.get("type", "native") if isinstance(settings, dict) else "native"
            enabled = settings.get("enabled", True) if isinstance(settings, dict) else bool(settings)
            if not enabled:
                continue
            if ptype == "standalone":
                continue  # handled by japm.py directly

            plugin_config = settings if isinstance(settings, dict) else {}
            self._load_one(name, plugin_config)

    def _load_one(self, name, plugin_config):
        mod_path = "plugins.%s" % name
        try:
            mod = importlib.import_module(mod_path)
        except ModuleNotFoundError:
            LOG.warning("Plugin module not found: %s", mod_path)
            return None

        cls = None
        for attr in dir(mod):
            obj = getattr(mod, attr)
            if isinstance(obj, type) and issubclass(obj, BasePlugin) and obj is not BasePlugin:
                cls = obj
                break

        if cls is None:
            LOG.warning("No BasePlugin subclass found in %s", mod_path)
            return None

        inst = cls()
        try:
            if not inst.on_init(self.api, plugin_config):
                LOG.warning("Plugin %s refused to initialize", name)
                return None
        except Exception:
            LOG.error("Error initializing plugin %s:\n%s", name, traceback.format_exc())
            return None

        self.plugins.append(inst)
        LOG.info("Loaded plugin: %s (v%s)", name, getattr(inst, "version", "?"))
        return inst

    def start_all(self):
        for p in self.plugins:
            try:
                p.on_start()
            except Exception:
                LOG.error("Error starting plugin %s:\n%s", p.name, traceback.format_exc())

    def dispatch(self, event):
        for p in self.plugins:
            try:
                if p.on_event(event):
                    return
            except Exception:
                LOG.error("Error in plugin %s on_event:\n%s", p.name, traceback.format_exc())

    def loop_all(self):
        for p in self.plugins:
            try:
                p.on_loop()
            except Exception:
                LOG.error("Error in plugin %s on_loop:\n%s", p.name, traceback.format_exc())

    def finish_all(self):
        for p in self.plugins:
            try:
                p.on_finish()
            except Exception:
                pass
