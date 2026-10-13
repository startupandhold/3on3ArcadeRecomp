#!/usr/bin/env python3
"""Log what the game sends to its online servers.

Run this on the machine named by the game's --online_server, then start the
game with --online=true and enter the XBOX LIVE menu. Every TCP connection and
UDP datagram is hex-dumped and also appended to online_capture.log. Packets
that look like Blaze (Fire) frames or EA lobby text frames are decoded.

Usage: online_capture.py [--tcp PORTS] [--udp PORTS] [--bind ADDR]
"""
import argparse
import datetime
import selectors
import socket
import struct

DEFAULT_TCP = "80,443,3658,10041,10744,10899,10901,30380,42100,42127,8080,9946"
LOBBY_PORT = 10901
# Off by default: games on this PC bind these ports for peer-to-peer traffic.
# Capture with --udp 1000,3658,3659,17500 when the server runs elsewhere.
DEFAULT_UDP = ""

log_file = open("online_capture.log", "a", buffering=1)


def log(msg):
    line = f"{datetime.datetime.now():%H:%M:%S.%f} {msg}"
    print(line)
    log_file.write(line + "\n")


def hexdump(data, limit=512):
    out = []
    for off in range(0, min(len(data), limit), 16):
        chunk = data[off:off + 16]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append(f"    {off:04x}  {hexpart:<47}  {text}")
    if len(data) > limit:
        out.append(f"    ... {len(data) - limit} more bytes")
    return "\n".join(out)


def describe(data):
    notes = []
    # Blaze Fire frame: u16 size, u16 component, u16 command, u16 error,
    # u16 type/flags, u16 msg id (all big-endian), then TDF payload.
    if len(data) >= 12:
        size, comp, cmd, err, qtype, msgid = struct.unpack(">6H", data[:12])
        if size <= len(data) - 12 + 0x10000 and comp < 0x8000 and cmd < 0x1000:
            notes.append(
                f"blaze? size={size} component=0x{comp:04x} command=0x{cmd:04x} "
                f"error=0x{err:04x} type=0x{qtype:04x} id={msgid}")
    # EA lobby / plasma frame: 4-char command, u32 flags, u32 length, text.
    if len(data) >= 12 and all(65 <= b <= 90 or 97 <= b <= 122 or b == 64 for b in data[:4]):
        length = struct.unpack(">I", data[8:12])[0]
        body = data[12:length].split(b"\0")[0].decode("latin1", "replace")
        notes.append(f"ea-lobby cmd={data[:4].decode()} len={length} body={body!r}")
    return notes


def lobby_frames(data):
    """Splits EA lobby frames: 4-char command, u32 flags, u32 total length."""
    while len(data) >= 12:
        length = struct.unpack(">I", data[8:12])[0]
        if length < 12 or length > len(data):
            break
        yield data[:4], data[12:length].split(b"\0")[0]
        data = data[length:]


def lobby_frame(cmd, fields, sep="\n"):
    body = sep.join(f"{k}={v}" for k, v in fields.items()).encode("latin1") + b"\0"
    return cmd + b"\0\0\0\0" + struct.pack(">I", 12 + len(body)) + body


def lobby_fields(body):
    """Parses KEY=VALUE pairs; most requests use newlines, 'sele' uses spaces."""
    text = body.decode("latin1")
    fields = {}
    for line in text.split("\n" if "\n" in text else " "):
        key, sep, value = line.partition("=")
        if sep:
            fields[key] = value
    return fields


LKEY = "3fcf27540c92935b0a66fd3b0000283c"
ROOM_ID = 1
RANKED_SYSFLAG = 1 << 18
# Consoles per quick match (--match-size). The game asks for 2; more is the
# experimental team play mode, with every console in one game.
match_size = 2


class Client:
    """One lobby connection (one game instance)."""
    next_id = 1

    def __init__(self, sock, peer_ip):
        self.sock = sock
        self.id = Client.next_id
        Client.next_id += 1
        self.persona = "Player"
        self.peer_ip = peer_ip
        self.mac_ip = None
        self.maddr = ""
        self.game = None
        self.userflags = "0"
        self.userparams = ""
        # The client's guest list exactly as its gset GUEST sent it, e.g.
        # Alpha(1) or "Alpha(1),Alpha(2)"; returned verbatim as OPGUEST. The
        # game maps each guest's "(N)" suffix to its controller, so any other
        # value (even "0") becomes a guest with no controller.
        self.guest_field = ""

    @property
    def ip(self):
        """The address other players reach this instance on. The game's MAC is
        00:22 followed by the advertised IPv4 address (see FillXnAddr)."""
        return self.mac_ip or self.peer_ip

    def learn(self, request):
        mac = request.get("MAC", "").lstrip("$")
        if len(mac) == 12 and mac.startswith("0022"):
            self.mac_ip = ".".join(str(int(mac[i:i + 2], 16)) for i in range(4, 12, 2))
        if request.get("MADDR"):
            self.maddr = request["MADDR"]


class Game:
    next_id = 1

    def __init__(self, request, players):
        self.id = Game.next_id
        Game.next_id += 1
        self.request = request
        self.players = players
        self.when = datetime.datetime.now()
        for p in players:
            p.game = self


clients = {}
quick_queue = []
# (due time, action) pairs run by the main loop.
timers = []
# The game only starts listening for +who/+mgm once it has handled the gqwk
# reply, which can take a frame; notifications that arrive in the same read
# as the reply are missed and the search never ends.
MATCH_NOTIFY_DELAY = 1.0


def later(delay, action):
    timers.append((datetime.datetime.now() + datetime.timedelta(seconds=delay), action))


def run_timers():
    now = datetime.datetime.now()
    due = [t for t in timers if t[0] <= now]
    for t in due:
        timers.remove(t)
        t[1]()
    if not timers:
        return None
    return max(0.0, (min(t[0] for t in timers) - now).total_seconds())


# gset fields from the host that change the game everyone sees.
GAME_SETTINGS = {"SYSFLAGS", "CUSTFLAGS", "PARAMS", "PLATPARAMS", "SESS", "PRIV",
                 "MINSIZE", "MAXSIZE"}


def game_info(game):
    """Game description shared by +mgm (membership) and +ses (start), after
    ea-nation-server's GameUtils.getGameInfo, plus each player's MADDR, which
    carries the XNADDR an Xbox client needs to reach its peer."""
    req = game.request
    sysflags = req.get("SYSFLAGS", "0")
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
        "MINSIZE": match_size if match_size > 2 else req.get("MINSIZE", 2),
        "MAXSIZE": match_size if match_size > 2 else req.get("MAXSIZE", 2),
        "NUMPART": 1,
        "SEED": game.id * 7919,
        "WHEN": f"{game.when:%Y.%m.%d-%H:%M:%S}",
        "AUTH": "098f6bcd4621d373cade4e832627b4f6" if int(sysflags) & RANKED_SYSFLAG else "",
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
            f"PARTSIZE{i}": match_size if match_size > 2 else req.get("MAXSIZE", 2),
            f"PARTPARAMS{i}": "",
        })
    return info


def send(client, frame):
    log(f"tcp -> {client.persona}#{client.id} {frame[:4].decode()}\n" + hexdump(frame))
    try:
        client.sock.sendall(frame)
    except OSError as e:
        log(f"send to {client.persona}#{client.id} failed: {e}")


def broadcast(game, cmd):
    info = game_info(game)
    for p in game.players:
        send(p, lobby_frame(cmd, info))


def leave_game(client):
    if client in quick_queue:
        quick_queue.remove(client)
    game, client.game = client.game, None
    client.guest_field = ""
    if not game:
        return
    game.players.remove(client)
    log(f"game {game.id}: {client.persona}#{client.id} left")
    send(client, who_frame(client))
    if game.players:
        broadcast(game, b"+mgm")


def quick_match(client, request):
    """gqwk MODE=2 waits until match_size players search; they are then put in
    one game, the first searcher hosting. MODE=3 cancels.

    The game's LobbyMatchupGameModule reports a match only after both its own
    +who shows a new game id (G) and a +mgm with the game's NAME arrives; the
    host then starts the game with gsta, which sends +ses."""
    mode = request.get("MODE")
    if mode == "3":
        if client in quick_queue:
            quick_queue.remove(client)
        log(f"quick match: {client.persona}#{client.id} cancelled")
        leave_game(client)
        return
    if mode != "2":
        return
    leave_game(client)
    client.userflags = "0"
    client.search = request
    if len(quick_queue) + 1 < match_size:
        quick_queue.append(client)
        log(f"quick match: {client.persona}#{client.id} waiting "
            f"({len(quick_queue)}/{match_size} searching)")
        return
    players = quick_queue[:match_size - 1] + [client]
    del quick_queue[:match_size - 1]
    game = Game(players[0].search, players)
    log(f"quick match: game {game.id} host {players[0].persona}#{players[0].id} with "
        + ", ".join(f"{p.persona}#{p.id} ({p.ip})" for p in players))

    def notify():
        if not game.players:
            return
        for p in game.players:
            send(p, who_frame(p))
        broadcast(game, b"+mgm")

    later(MATCH_NOTIFY_DELAY, notify)


def who_frame(client):
    """+who describes the logged-in persona; the SDK expects it after pers."""
    name = client.persona
    return lobby_frame(b"+who", {
        "I": client.id, "M": name, "N": name, "F": "U", "P": 80, "S": ",,,,,,,,,", "X": "",
        "G": client.game.id if client.game else 0,
        "AT": "", "CL": 511, "LV": 1049601, "MD": 0, "R": "", "US": "", "HW": 0, "RP": 0,
        "LO": "enUS", "CI": 0, "CT": 0, "A": client.ip, "LA": client.ip,
        # The client's own MADDR ($xuid^xnaddr); the lobby takes the persona's
        # XUID from it, and session setup fails while that XUID is zero.
        "MA": client.maddr,
        "C": "4000,,7,1,1,,1,1,5553", "RI": ROOM_ID, "RT": 1, "RG": 0, "RGC": 0,
        "RM": "LVL.1", "RF": "CK",
    })


def room_frame():
    return lobby_frame(b"+rom", {
        "I": ROOM_ID, "N": "LVL.1", "DN": "Lobby", "D": "", "F": "A", "T": 1, "L": 50,
    })


QUICK_MESSAGES = {
    "IMGATE": 0,
    "QMSG0": '"Do you want to play a game?"', "QMSG1": "Yes", "QMSG2": "No",
    "QMSG3": '"OK, let\'s start"', "QMSG4": '"Have fun!"', "QMSG5": '"Good game!"',
    "QMSG6": "Thanks!", "QMSG7": '"Catch you later"', "QMSG8": '"See Ya!"',
    "SPM_EA": 0, "SPM_PART": 0,
}


def lobby_reply(cmd, server_ip, request, client):
    """Returns the frames to send for a lobby request (replies follow the
    ea-nation-server emulator). Unknown commands are echoed back as success."""
    name = request.get("GTAG") or request.get("PERS") or request.get("NAME") or "Player"
    client_ip = client.ip
    if cmd in (b"@tic", b"addr"):
        return []
    if cmd == b"gqwk":
        echo = lobby_frame(cmd, {k: v for k, v in request.items() if k != "MADDR"})
        # The echo must reach the game before any +mgm/+ses from pairing.
        send(client, echo)
        quick_match(client, request)
        return []
    if cmd == b"gsta":
        if not client.game:
            return [lobby_frame(cmd, request)]
        send(client, lobby_frame(cmd, request))
        broadcast(client.game, b"+ses")
        return []
    if cmd == b"gset":
        client.userflags = request.get("USERFLAGS", client.userflags)
        client.userparams = request.get("USERPARAMS", client.userparams)
        if "GUEST" in request:
            client.guest_field = request["GUEST"]
            names = [g for g in client.guest_field.strip('"').split(",") if g]
            log(f"guests: {client.persona}#{client.id} now has {names}")
        if client.game:
            if client is client.game.players[0]:
                client.game.request.update(
                    {k: v for k, v in request.items() if k in GAME_SETTINGS})
            send(client, lobby_frame(cmd, game_info(client.game)))
            broadcast(client.game, b"+mgm")
            return []
        return [lobby_frame(cmd, request)]
    if cmd in (b"glea", b"gdel"):
        leave_game(client)
        return [lobby_frame(cmd, request)]
    if cmd == b"gget":
        return [lobby_frame(cmd, game_info(client.game) if client.game else {})]
    if cmd == b"auth":
        client.persona = name
        return [lobby_frame(b"auth", {
            "NAME": name,
            "PERSONAS": name,
            "GTAG": name,
            "XUID": request.get("XUID", "$0"),
            "LKEY": "3fcf27540c92935b0a66fd3b0000283c",
            "ADDR": client_ip,
            "TOS": 1,
            "SPAM": "NN",
            "MAIL": "player@localhost",
            "BORN": "19800101",
            "GEND": "M",
            "FROM": "US",
            "LANG": "en",
            "LOC": "enUS",
            "SINCE": "2008.11.1 12:00:00",
            "LAST": "2008.11.1 12:00:00",
        })]
    if cmd == b"pers":
        client.persona = name
        return [lobby_frame(b"pers", {
            "PERS": name,
            "NAME": name,
            "LKEY": LKEY,
            "EX-ticker": "",
            "LOC": "enUS",
            "A": client_ip,
            "LA": client_ip,
            "IDLE": 600000,
        }), room_frame(), who_frame(client)]
    if cmd == b"~png":
        return [lobby_frame(b"~png", request)]
    if cmd == b"@dir":
        return [lobby_frame(b"@dir", {
            "ADDR": server_ip,
            "PORT": LOBBY_PORT,
            "SESS": "1234567890",
            "MASK": "0295f3f70ecb1757cd7001b9a7a5eac8",
        })]
    if cmd == b"skey":
        return [lobby_frame(b"skey", {"SKEY": "$37940faf2a8d1381a3b7d0d2f570e6a7"})]
    if cmd == b"news" and request.get("NAME") == "osdk_core.cfg":
        return [lobby_frame(b"news", {
            "OSDK_LOBBY_PERMANENT": 1,
            "TELE_NABL": 0,
        })]
    if cmd == b"news":
        return [lobby_frame(b"news", {
            "BUDDY_SERVER": server_ip,
            "BUDDYSERVERNAME": server_ip,
            "BUDDY_PORT": 10899,
            "BUDDYPORT": 10899,
            "CONTEXT": "buddy",
            "TOSAC_URL": "http://localhost/tos/",
            "TOSA_URL": "http://localhost/tos/",
            "TOS_URL": "http://localhost/tos/",
            "NEWS_URL": "http://localhost/news/",
            "FAQ_URL": "http://localhost/faq/",
            "EACONNECT_WEBOFFER_URL": "http://localhost/eaconnect/",
            "ROSTER_URL": "http://localhost/roster",
            "ROSTER_VER": "1.0",
        })]
    if cmd == b"sele":
        fields = dict(request)
        fields.update({"MORE": 0, "SLOTS": 4, "STATS": request.get("STATS", 0)})
        return [lobby_frame(b"sele", fields, sep=" ")]
    if cmd == b"uatr":
        return [lobby_frame(b"uatr", request), who_frame(client)]
    if cmd == b"qdef":
        return [lobby_frame(b"qdef", QUICK_MESSAGES)]
    if cmd == b"usld":
        return [lobby_frame(b"usld", {**QUICK_MESSAGES, "UID": 1})]
    if cmd == b"slst":
        return [lobby_frame(b"slst", {"COUNT": 1, "VIEW0": 'Career,"My Career"'})]
    if cmd == b"rcat":
        return [lobby_frame(b"rcat", {"CAT0": "Default,Global,LVL,,,,,1"}), room_frame()]
    if cmd == b"cate":
        return [lobby_frame(b"cate", {
            "CC": 1, "IC": 1, "VC": 1, "SYMS": '"Top 100"', "SS": 8, "R": "0,1,1,1,1,1,1,1",
        })]
    return [lobby_frame(cmd, {k: v for k, v in request.items() if k not in ("PASS", "MADDR")})]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tcp", default=DEFAULT_TCP)
    ap.add_argument("--udp", default=DEFAULT_UDP)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--respond", action="store_true",
                    help="answer the lobby directory request (@dir) instead of only logging")
    ap.add_argument("--match-size", type=int, default=2,
                    help="consoles per quick match; above 2 is experimental team play")
    args = ap.parse_args()
    global match_size
    match_size = max(2, args.match_size)

    sel = selectors.DefaultSelector()
    for port in filter(None, args.tcp.split(",")):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((args.bind, int(port)))
        except OSError as e:
            log(f"tcp {port}: {e}")
            continue
        s.listen()
        s.setblocking(False)
        sel.register(s, selectors.EVENT_READ, ("listen", int(port)))
    for port in filter(None, args.udp.split(",")):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind((args.bind, int(port)))
        except OSError as e:
            log(f"udp {port}: {e}")
            continue
        s.setblocking(False)
        sel.register(s, selectors.EVENT_READ, ("udp", int(port)))
    log(f"listening tcp [{args.tcp}] udp [{args.udp}]")

    while True:
        for key, _ in sel.select(run_timers()):
            kind, port = key.data[:2]
            sock = key.fileobj
            if kind == "listen":
                conn, addr = sock.accept()
                conn.setblocking(False)
                sel.register(conn, selectors.EVENT_READ, ("tcp", port, addr))
                clients[conn] = Client(conn, addr[0])
                log(f"tcp :{port} connection from {addr[0]}:{addr[1]}")
            elif kind == "tcp":
                try:
                    data = sock.recv(65536)
                except ConnectionError:
                    data = b""
                if not data:
                    log(f"tcp :{port} closed by {key.data[2][0]}")
                    client = clients.pop(sock, None)
                    if client:
                        leave_game(client)
                    sel.unregister(sock)
                    sock.close()
                    continue
                log(f"tcp :{port} {len(data)} bytes")
                for note in describe(data):
                    log("  " + note)
                log("\n" + hexdump(data))
                if args.respond:
                    server_ip = sock.getsockname()[0]
                    client = clients[sock]
                    for cmd, body in lobby_frames(data):
                        request = lobby_fields(body)
                        client.learn(request)
                        for reply in lobby_reply(cmd, server_ip, request, client):
                            send(client, reply)
            else:
                data, addr = sock.recvfrom(65536)
                log(f"udp :{port} {len(data)} bytes from {addr[0]}:{addr[1]}")
                for note in describe(data):
                    log("  " + note)
                log("\n" + hexdump(data))


if __name__ == "__main__":
    main()
