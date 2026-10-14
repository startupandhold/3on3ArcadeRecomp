"""EA lobby (DirtySDK "plasma"-style) server: login, quick match and game
membership for NHL 3 on 3. Replies follow the ea-nation-server emulator.

Frames: 4-char command, u32 flags, u32 total length (big-endian), then
KEY=VALUE text (newline-separated; 'sele' uses spaces), NUL-terminated.
"""

import asyncio
import datetime
import logging
import struct

log = logging.getLogger("lobby")

LOBBY_PORT = 10901
LKEY = "3fcf27540c92935b0a66fd3b0000283c"
ROOM_ID = 1
RANKED_SYSFLAG = 1 << 18
MAX_FRAME = 64 * 1024
# Unranked quick match is team play: a game opens once two players search, and
# later searchers join it (on Select Sides) until the host starts it, up to
# this many consoles and players (guests included; 3 per side).
TEAM_MAX_CONSOLES = 6
TEAM_MAX_PLAYERS = 6
# The game only starts listening for +who/+mgm once it has handled the gqwk
# reply, which can take a frame; notifications that arrive in the same read
# as the reply are missed and the search never ends.
MATCH_NOTIFY_DELAY = 1.0
# gset fields from the host that change the game everyone sees.
GAME_SETTINGS = {"SYSFLAGS", "CUSTFLAGS", "PARAMS", "PLATPARAMS", "SESS", "PRIV",
                 "MINSIZE", "MAXSIZE"}
QUICK_MESSAGES = {
    "IMGATE": 0,
    "QMSG0": '"Do you want to play a game?"', "QMSG1": "Yes", "QMSG2": "No",
    "QMSG3": '"OK, let\'s start"', "QMSG4": '"Have fun!"', "QMSG5": '"Good game!"',
    "QMSG6": "Thanks!", "QMSG7": '"Catch you later"', "QMSG8": '"See Ya!"',
    "SPM_EA": 0, "SPM_PART": 0,
}


def lobby_frame(cmd, fields, sep="\n"):
    body = sep.join(f"{k}={v}" for k, v in fields.items()).encode("latin1") + b"\0"
    return cmd + b"\0\0\0\0" + struct.pack(">I", 12 + len(body)) + body


def lobby_fields(body):
    text = body.decode("latin1")
    fields = {}
    for line in text.split("\n" if "\n" in text else " "):
        key, sep, value = line.partition("=")
        if sep:
            fields[key] = value
    return fields


def sanitize(value, limit=64):
    """Strips characters that would break the KEY=VALUE framing."""
    return "".join(c for c in value if c not in "\0\n\r=")[:limit]


class Client:
    """One lobby connection (one game instance)."""
    next_id = 1

    def __init__(self, lobby, writer, peer_ip):
        self.lobby = lobby
        self.writer = writer
        self.id = Client.next_id
        Client.next_id += 1
        self.persona = "Player"
        self.signed_in = False  # persona chosen at auth, unique among signed-in players
        self.peer_ip = peer_ip
        self.mac_ip = None
        self.maddr = ""
        self.game = None
        self.search = {}
        self.userflags = "0"
        self.userparams = ""
        # The guest list exactly as gset GUEST sent it, e.g. "Alpha(1),Alpha(2)";
        # returned verbatim as OPGUEST. The game maps each "(N)" suffix to a
        # controller, so any other value becomes a guest with no controller.
        self.guest_field = ""

    @property
    def name(self):
        return f"{self.persona}#{self.id}"

    @property
    def users(self):
        """Players on this console: its gamertag plus its guests."""
        return 1 + len([g for g in self.guest_field.strip('"').split(",") if g])

    @property
    def ip(self):
        """The address other players reach this instance on: the game's MAC is
        00:22 followed by its advertised IPv4 address (with the tunnel, the
        virtual address the server assigned)."""
        return self.mac_ip or self.peer_ip

    def learn(self, request):
        mac = request.get("MAC", "").lstrip("$")
        if len(mac) == 12 and mac.startswith("0022"):
            try:
                self.mac_ip = ".".join(str(int(mac[i:i + 2], 16)) for i in range(4, 12, 2))
            except ValueError:
                pass
        if request.get("MADDR"):
            self.maddr = sanitize(request["MADDR"], 256)

    def send(self, frame):
        if self.writer.is_closing():
            return
        self.writer.write(frame)


class Game:
    next_id = 1

    def __init__(self, request, players, ranked):
        self.id = Game.next_id
        Game.next_id += 1
        self.request = dict(request)
        self.players = list(players)
        self.ranked = ranked
        # Started (gsta) or lost its host: no one else may join.
        self.closed = False
        self.when = datetime.datetime.now()
        for p in self.players:
            p.game = self

    def open_to(self, client):
        return (not self.ranked and not self.closed and self.players
                and len(self.players) < TEAM_MAX_CONSOLES
                and sum(p.users for p in self.players) + client.users <= TEAM_MAX_PLAYERS)


class Lobby:
    def __init__(self, public_ip):
        self.public_ip = public_ip
        self.clients = set()
        # Searching clients, one queue per kind (ranked or not).
        self.queues = {True: [], False: []}
        # Unranked games still taking players.
        self.open_games = []

    # -- queries used by the tunnel/relay --------------------------------

    def game_of_ip(self, ip):
        for c in self.clients:
            if c.ip == ip and c.game:
                return c.game
        return None

    def status(self):
        games = {id(c.game): c.game for c in self.clients if c.game}
        return {
            "players": len(self.clients),
            "searching": sum(len(q) for q in self.queues.values()),
            "games": len(games),
            "open_games": len(self.open_games),
        }

    # -- connection handling ----------------------------------------------

    async def handle(self, reader, writer):
        peer = writer.get_extra_info("peername") or ("?", 0)
        client = Client(self, writer, peer[0])
        self.clients.add(client)
        log.info("connection %s from %s:%s", client.id, peer[0], peer[1])
        buffer = b""
        try:
            while True:
                data = await asyncio.wait_for(reader.read(65536), timeout=900)
                if not data:
                    break
                buffer += data
                while len(buffer) >= 12:
                    length = struct.unpack(">I", buffer[8:12])[0]
                    if length < 12 or length > MAX_FRAME:
                        raise ValueError(f"bad frame length {length}")
                    if len(buffer) < length:
                        break
                    cmd, body = buffer[:4], buffer[12:length].split(b"\0")[0]
                    buffer = buffer[length:]
                    request = lobby_fields(body)
                    client.learn(request)
                    log.debug("%s -> %s %s", client.name, cmd, request)
                    for reply in self.reply(cmd, request, client):
                        client.send(reply)
                if len(buffer) > MAX_FRAME:
                    raise ValueError("oversized frame")
                await writer.drain()
        except (asyncio.TimeoutError, ConnectionError, ValueError) as e:
            log.info("%s dropped: %s", client.name, e or type(e).__name__)
        finally:
            self.leave_game(client)
            self.clients.discard(client)
            log.info("%s disconnected", client.name)
            writer.close()

    # -- games ------------------------------------------------------------

    def game_info(self, game):
        """Shared by +mgm (membership) and +ses (start), after ea-nation-server's
        GameUtils.getGameInfo, plus each player's MADDR, which carries the
        XNADDR a client needs to reach its peer."""
        req = game.request
        sysflags = req.get("SYSFLAGS", "0")
        ranked = game.ranked
        # A team play game is always full: its size follows its members, as
        # players join it.
        size = max(2, len(game.players))
        team = not ranked
        info = {
            "IDENT": game.id,
            "NAME": req.get("NAME", game.players[0].persona),
            "HOST": game.players[0].persona,
            "PARAMS": req.get("PARAMS", ""),
            "PLATPARAMS": req.get("PLATPARAMS", 0),
            "ROOM": 0,
            "CUSTFLAGS": 0,
            "SYSFLAGS": sysflags,
            "COUNT": len(game.players),
            "PRIV": req.get("PRIV", 0),
            "MINSIZE": size if team else req.get("MINSIZE", 2),
            "MAXSIZE": size if team else req.get("MAXSIZE", 2),
            "NUMPART": 1,
            "SEED": game.id * 7919,
            "WHEN": f"{game.when:%Y.%m.%d-%H:%M:%S}",
            "AUTH": "098f6bcd4621d373cade4e832627b4f6" if ranked else "",
            # The host's XSESSION_INFO once it has created the session; joiners
            # wait while it is empty and fail on anything else.
            "SESS": req.get("SESS", ""),
            "EVID": 0,
            "EVGID": 0,
        }
        for i, p in enumerate(game.players):
            info.update({
                f"OPID{i}": p.id,
                f"OPPO{i}": p.persona,
                f"ADDR{i}": p.ip,
                f"LADDR{i}": p.ip,
                f"MADDR{i}": p.maddr,
                f"OPPART{i}": 0,
                f"OPPARAM{i}": p.userparams,
                f"OPFLAG{i}": p.userflags,
                f"PRES{i}": 0,
                f"OPGUEST{i}": p.guest_field,
                f"PARTSIZE{i}": size if team else req.get("MAXSIZE", 2),
                f"PARTPARAMS{i}": "",
            })
        return info

    def broadcast(self, game, cmd):
        info = self.game_info(game)
        for p in game.players:
            p.send(lobby_frame(cmd, info))

    def leave_queue(self, client):
        for queue in self.queues.values():
            if client in queue:
                queue.remove(client)

    def leave_game(self, client):
        self.leave_queue(client)
        game, client.game = client.game, None
        client.guest_field = ""
        if not game:
            return
        host_left = game.players and game.players[0] is client
        game.players.remove(client)
        log.info("game %d: %s left", game.id, client.name)
        client.send(self.who_frame(client))
        if host_left or not game.players:
            self.close(game)
        if game.players:
            self.broadcast(game, b"+mgm")

    def close(self, game):
        game.closed = True
        if game in self.open_games:
            self.open_games.remove(game)

    def quick_match(self, client, request):
        """gqwk MODE=2 searches, MODE=3 cancels.

        Ranked pairs two searchers. Unranked (team play) opens a game as soon
        as two players search, the first one hosting; anyone who searches
        while it is open joins it and lands on Select Sides with the others.
        The host starts the game (gsta, which sends +ses) once everyone is
        ready, which closes it to new players.

        The game reports a match once its +who shows the game id and a +mgm
        with the game's NAME arrives."""
        mode = request.get("MODE")
        if mode == "3":
            log.info("quick match: %s cancelled", client.name)
            self.leave_game(client)
            return
        if mode != "2":
            return
        self.leave_game(client)
        client.userflags = "0"
        client.search = request
        try:
            ranked = bool(int(request.get("SYSFLAGS", "0")) & RANKED_SYSFLAG)
        except ValueError:
            ranked = False
        kind = "ranked" if ranked else "team play"
        if not ranked:
            game = next((g for g in self.open_games if g.open_to(client)), None)
            if game:
                game.players.append(client)
                client.game = game
                log.info("quick match (%s): %s joins game %d (%d consoles: %s)", kind,
                         client.name, game.id, len(game.players),
                         ", ".join(p.name for p in game.players))
                self.notify_later(game, [client])
                return
        queue = self.queues[ranked]
        if not queue:
            queue.append(client)
            log.info("quick match (%s): %s waiting", kind, client.name)
            return
        players = [queue.pop(0), client]
        game = Game(players[0].search, players, ranked)
        if not ranked:
            self.open_games.append(game)
        log.info("quick match (%s): game %d: %s", kind, game.id,
                 ", ".join(f"{p.name} ({p.ip})" for p in players))
        self.notify_later(game, players)

    def notify_later(self, game, joined):
        """Tells `joined` they are in `game` (+who) and everyone the new
        membership (+mgm), once their search reply has been handled."""

        def notify():
            if not game.players:
                return
            for p in joined:
                if p.game is game:
                    p.send(self.who_frame(p))
            self.broadcast(game, b"+mgm")

        asyncio.get_running_loop().call_later(MATCH_NOTIFY_DELAY, notify)

    def who_frame(self, client):
        """+who describes the logged-in persona; the SDK expects it after pers."""
        name = client.persona
        return lobby_frame(b"+who", {
            "I": client.id, "M": name, "N": name, "F": "U", "P": 80, "S": ",,,,,,,,,", "X": "",
            "G": client.game.id if client.game else 0,
            "AT": "", "CL": 511, "LV": 1049601, "MD": 0, "R": "", "US": "", "HW": 0, "RP": 0,
            "LO": "enUS", "CI": 0, "CT": 0, "A": client.ip, "LA": client.ip,
            # The client's own MADDR ($xuid^xnaddr); the lobby takes the
            # persona's XUID from it, and session setup fails while it is zero.
            "MA": client.maddr,
            "C": "4000,,7,1,1,,1,1,5553", "RI": ROOM_ID, "RT": 1, "RG": 0, "RGC": 0,
            "RM": "LVL.1", "RF": "CK",
        })

    @staticmethod
    def room_frame():
        return lobby_frame(b"+rom", {
            "I": ROOM_ID, "N": "LVL.1", "DN": "Lobby", "D": "", "F": "A", "T": 1, "L": 50,
        })

    # -- requests ---------------------------------------------------------

    def unique_persona(self, name, client):
        """`name`, or `name` with the lowest number from 2 up that makes it
        unique among the other signed-in players (case-insensitive), cut to
        stay within 15 characters."""
        taken = {c.persona.lower() for c in self.clients if c is not client and c.signed_in}
        candidate, n = name, 1
        while candidate.lower() in taken:
            n += 1
            candidate = name[:15 - len(str(n))] + str(n)
        return candidate

    def reply(self, cmd, request, client):
        """Returns the frames to send for a request. Unknown commands are echoed
        back as success."""
        name = sanitize(request.get("GTAG") or request.get("PERS") or request.get("NAME")
                        or "Player", 15) or "Player"
        if cmd in (b"@tic", b"addr"):
            return []
        if cmd == b"gqwk":
            # The echo must reach the game before any +mgm/+ses from pairing.
            client.send(lobby_frame(cmd, {k: v for k, v in request.items() if k != "MADDR"}))
            self.quick_match(client, request)
            return []
        if cmd == b"gsta":
            if not client.game:
                return [lobby_frame(cmd, request)]
            client.send(lobby_frame(cmd, request))
            if client.game.players[0] is client and not client.game.closed:
                self.close(client.game)
                log.info("game %d: started with %d consoles", client.game.id,
                         len(client.game.players))
            self.broadcast(client.game, b"+ses")
            return []
        if cmd == b"gset":
            client.userflags = sanitize(request.get("USERFLAGS", client.userflags))
            client.userparams = sanitize(request.get("USERPARAMS", client.userparams), 256)
            if "GUEST" in request:
                client.guest_field = sanitize(request["GUEST"], 128)
            if client.game:
                if client is client.game.players[0]:
                    client.game.request.update(
                        {k: v for k, v in request.items() if k in GAME_SETTINGS})
                client.send(lobby_frame(cmd, self.game_info(client.game)))
                self.broadcast(client.game, b"+mgm")
                return []
            return [lobby_frame(cmd, request)]
        if cmd in (b"glea", b"gdel"):
            self.leave_game(client)
            return [lobby_frame(cmd, request)]
        if cmd == b"gget":
            return [lobby_frame(cmd, self.game_info(client.game) if client.game else {})]
        if cmd == b"auth":
            client.persona = self.unique_persona(name, client)
            client.signed_in = True
            if client.persona != name:
                log.info("%s: gamertag %s is in use, renamed", client.name, name)
            log.info("%s signed in from %s", client.name, client.ip)
            name = client.persona
            return [lobby_frame(b"auth", {
                "NAME": name, "PERSONAS": name, "GTAG": name,
                "XUID": sanitize(request.get("XUID", "$0")),
                "LKEY": LKEY, "ADDR": client.ip, "TOS": 1, "SPAM": "NN",
                "MAIL": "player@localhost", "BORN": "19800101", "GEND": "M", "FROM": "US",
                "LANG": "en", "LOC": "enUS",
                "SINCE": "2008.11.1 12:00:00", "LAST": "2008.11.1 12:00:00",
            })]
        if cmd == b"pers":
            if client.signed_in:
                name = client.persona
            else:
                client.persona = name = self.unique_persona(name, client)
                client.signed_in = True
            return [lobby_frame(b"pers", {
                "PERS": name, "NAME": name, "LKEY": LKEY, "EX-ticker": "", "LOC": "enUS",
                "A": client.ip, "LA": client.ip, "IDLE": 600000,
            }), self.room_frame(), self.who_frame(client)]
        if cmd == b"~png":
            return [lobby_frame(b"~png", request)]
        if cmd == b"@dir":
            return [lobby_frame(b"@dir", {
                "ADDR": self.public_ip, "PORT": LOBBY_PORT,
                "SESS": "1234567890", "MASK": "0295f3f70ecb1757cd7001b9a7a5eac8",
            })]
        if cmd == b"skey":
            return [lobby_frame(b"skey", {"SKEY": "$37940faf2a8d1381a3b7d0d2f570e6a7"})]
        if cmd == b"news" and request.get("NAME") == "osdk_core.cfg":
            return [lobby_frame(b"news", {"OSDK_LOBBY_PERMANENT": 1, "TELE_NABL": 0})]
        if cmd == b"news":
            return [lobby_frame(b"news", {
                "BUDDY_SERVER": self.public_ip, "BUDDYSERVERNAME": self.public_ip,
                "BUDDY_PORT": 10899, "BUDDYPORT": 10899, "CONTEXT": "buddy",
                "TOSAC_URL": "http://localhost/tos/", "TOSA_URL": "http://localhost/tos/",
                "TOS_URL": "http://localhost/tos/", "NEWS_URL": "http://localhost/news/",
                "FAQ_URL": "http://localhost/faq/",
                "EACONNECT_WEBOFFER_URL": "http://localhost/eaconnect/",
                "ROSTER_URL": "http://localhost/roster", "ROSTER_VER": "1.0",
            })]
        if cmd == b"sele":
            fields = dict(request)
            fields.update({"MORE": 0, "SLOTS": 4, "STATS": request.get("STATS", 0)})
            return [lobby_frame(b"sele", fields, sep=" ")]
        if cmd == b"uatr":
            return [lobby_frame(b"uatr", request), self.who_frame(client)]
        if cmd == b"qdef":
            return [lobby_frame(b"qdef", QUICK_MESSAGES)]
        if cmd == b"usld":
            return [lobby_frame(b"usld", {**QUICK_MESSAGES, "UID": 1})]
        if cmd == b"slst":
            return [lobby_frame(b"slst", {"COUNT": 1, "VIEW0": 'Career,"My Career"'})]
        if cmd == b"rcat":
            return [lobby_frame(b"rcat", {"CAT0": "Default,Global,LVL,,,,,1"}),
                    self.room_frame()]
        if cmd == b"cate":
            return [lobby_frame(b"cate", {
                "CC": 1, "IC": 1, "VC": 1, "SYMS": '"Top 100"', "SS": 8, "R": "0,1,1,1,1,1,1,1",
            })]
        return [lobby_frame(cmd, {k: v for k, v in request.items()
                                  if k not in ("PASS", "MADDR")})]
