EVENT_SERVER_STARTUP = 1
EVENT_SERVER_SHUTDOWN = 2
EVENT_MAP_CHANGE = 3
EVENT_PLAYER_CONNECT = 10
EVENT_PLAYER_DISCONNECT = 11
EVENT_PLAYER_BEGIN = 12
EVENT_PLAYER_CHAT = 20
EVENT_PLAYER_CHAT_TEAM = 21
EVENT_PLAYER_COMMAND = 22
EVENT_PLAYER_KILL = 30
EVENT_PLAYER_SPAWN = 31
EVENT_ROUND_EXIT = 40
EVENT_SERVER_EMPTY = 41
EVENT_TICK = 99


class Event:
    def __init__(self, type, data=None):
        self.type = type
        self.data = data or {}


class PlayerChatEvent(Event):
    def __init__(self, player_id, player_name, message, team=False):
        super().__init__(EVENT_PLAYER_CHAT_TEAM if team else EVENT_PLAYER_CHAT, {
            "player_id": player_id,
            "player_name": player_name,
            "message": message,
        })
        self.is_command = message.startswith("!")
        if self.is_command:
            self.type = EVENT_PLAYER_COMMAND
            parts = message[1:].split(None, 1)
            self.data["command"] = parts[0].lower() if parts else ""
            self.data["args"] = parts[1] if len(parts) > 1 else ""


class PlayerKillEvent(Event):
    def __init__(self, killer_id, victim_id, weapon):
        super().__init__(EVENT_PLAYER_KILL, {
            "killer_id": killer_id,
            "victim_id": victim_id,
            "weapon": weapon,
        })


class MapChangeEvent(Event):
    def __init__(self, map_name, old_map=""):
        super().__init__(EVENT_MAP_CHANGE, {"map": map_name, "old_map": old_map})
