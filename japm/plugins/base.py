"""Base class for native plugins."""


class BasePlugin:
    name = ""
    version = ""
    author = ""

    def on_init(self, api, config):
        """Called when plugin is first loaded.
           api      — object with say(), rcon(), cvar(), players(), etc.
           config   — dict of this plugin's settings from the JSON config.
           Return True on success, False to abort loading.
        """
        return True

    def on_start(self):
        """Called after all plugins have been initialized."""
        pass

    def on_event(self, event):
        """Called for each game event.
           Return True to stop the event from reaching other plugins.
        """
        return False

    def on_loop(self):
        """Called every tick (~50ms) while the server is running."""
        pass

    def on_finish(self):
        """Called during shutdown."""
        pass
