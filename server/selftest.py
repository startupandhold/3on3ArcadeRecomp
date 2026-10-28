#!/usr/bin/env python3
"""Exercises a running server the way game instances would: tunnel
registration and forwarding, ranked and team play quick matches (a team play
game opens with two players, a third joins it, and starting it closes it),
lockstep relay frames, renaming a gamertag that is already signed in, and a
created session that is listed, joined and closed when its host leaves.

    python selftest.py [host]
"""

import socket
import struct
import sys
import time

from lobby import RANKED_SYSFLAG, lobby_fields, lobby_frame
from tunnel import (HEADER, K_DATA, K_HELLO, K_RELAY, K_WELCOME, RELAY_HEADER, RELAY_MAGIC,
                    R_FRAMES, R_INPUTS, R_MESSAGES, TUNNEL_MAGIC, format_ip)

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
RELAY_SLOTS = 3


class Player:
    def __init__(self, name, roster=None):
        self.name = name
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.settimeout(2)
        hello = struct.pack("<II", 1234, 0)
        if roster is not None:
            hello += struct.pack("<I", roster)
        self.udp.sendto(HEADER.pack(TUNNEL_MAGIC, K_HELLO) + hello, (HOST, 3670))
        data = self.udp.recv(2048)
        magic, kind = HEADER.unpack_from(data)
        nonce, self.vip = struct.unpack_from("<II", data, HEADER.size)
        assert magic == TUNNEL_MAGIC and kind == K_WELCOME and nonce == 1234, data
        self.tcp = socket.create_connection((HOST, 10901))
        mac = "$0022" + "".join(f"{b:02x}" for b in struct.pack(">I", self.vip))
        self.tcp.sendall(lobby_frame(b"auth", {"GTAG": name, "MAC": mac}))
        self.tcp.sendall(lobby_frame(b"pers", {"PERS": name, "MAC": mac}))
        self.buffer = b""

    def frames(self, timeout):
        """Lobby frames received within `timeout` seconds."""
        self.tcp.settimeout(0.2)
        end = time.time() + timeout
        out = []
        while time.time() < end:
            try:
                self.buffer += self.tcp.recv(65536)
            except socket.timeout:
                pass
            while len(self.buffer) >= 12:
                length = struct.unpack(">I", self.buffer[8:12])[0]
                if len(self.buffer) < length:
                    break
                # An error reply's command includes its status, e.g. b"gjoiugam".
                cmd = self.buffer[:4] if self.buffer[4:8] == b"\0\0\0\0" else self.buffer[:8]
                # Chat (+msg) separates its fields with tabs.
                body = self.buffer[12:length].split(b"\0")[0].replace(b"\t", b"\n")
                out.append((cmd, lobby_fields(body)))
                self.buffer = self.buffer[length:]
        return out

    def last_mgm(self, timeout=2.5):
        mgm = [f for c, f in self.frames(timeout) if c == b"+mgm"]
        return mgm[-1] if mgm else None

    def search(self, ranked=False):
        self.frames(0.3)
        self.tcp.sendall(lobby_frame(b"gqwk", {
            "MODE": 2, "SYSFLAGS": RANKED_SYSFLAG if ranked else 0}))

    def enter_room(self):
        self.tcp.sendall(lobby_frame(b"move", {"IDENT": 1, "NAME": "LVL.1"}))
        self.frames(0.5)


def relay_packet(kind, slot, a, b, items):
    out = bytearray(RELAY_HEADER.pack(RELAY_MAGIC, kind, slot, RELAY_SLOTS, 0))
    out += struct.pack("<IIH", a, b, len(items))
    for k, d in items:
        out += struct.pack("<BH", k, len(d)) + d
    return HEADER.pack(TUNNEL_MAGIC, K_RELAY) + bytes(out)


def main():
    a, b, c, d = (Player(n) for n in ("Alpha", "Bravo", "Charlie", "Delta"))
    print("virtual addresses:", ", ".join(format_ip(p.vip) for p in (a, b, c, d)))

    a.udp.sendto(HEADER.pack(TUNNEL_MAGIC, K_DATA) + struct.pack("<IHH", b.vip, 3659, 3659)
                 + b"hello", (HOST, 3670))
    data = b.udp.recv(2048)
    src, _, dport = struct.unpack_from("<IHH", data, HEADER.size)
    assert src == a.vip and dport == 3659 and data[HEADER.size + 8:] == b"hello", data
    print("tunnel forwarding ok")

    # Ranked: two searchers make a 1v1.
    c.search(ranked=True)
    d.search(ranked=True)
    mgm = c.last_mgm()
    assert mgm and mgm["COUNT"] == "2" and mgm["MAXSIZE"] == "2", mgm
    c.tcp.sendall(lobby_frame(b"glea", {}))
    d.tcp.sendall(lobby_frame(b"glea", {}))
    print("ranked 1v1 ok")

    # Team play: Alpha and Bravo open a game; Charlie joins it.
    a.search()
    b.search()
    mgm = a.last_mgm()
    assert mgm and mgm["COUNT"] == "2" and mgm["MAXSIZE"] == "2", mgm
    game_id = mgm["IDENT"]
    c.search()
    mgm_a, mgm_c = a.last_mgm(), c.last_mgm()
    assert mgm_a and mgm_a["COUNT"] == "3" and mgm_a["MAXSIZE"] == "3", mgm_a
    assert mgm_c and mgm_c["IDENT"] == game_id and mgm_c["OPPO2"] == "Charlie", mgm_c
    print("team play: third player joined the open game")

    # Relay: every slot gets frame 0 with all inputs, and the others' messages.
    players = (a, b, c)
    for slot, p in enumerate(players):
        p.udp.sendto(relay_packet(R_INPUTS, slot, 0, 0, [(2, bytes([slot]) * 4)]), (HOST, 3670))
        p.udp.sendto(relay_packet(R_MESSAGES, slot, 0, 0, [(7, b"ready%d" % slot)]),
                     (HOST, 3670))
    for slot, p in enumerate(players):
        got_frame, messages = False, set()
        end = time.time() + 2
        while (not got_frame or len(messages) < RELAY_SLOTS - 1) and time.time() < end:
            body = p.udp.recv(2048)[HEADER.size:]
            _, kind, _, count, _ = RELAY_HEADER.unpack_from(body)
            _, first, n = struct.unpack_from("<IIH", body, RELAY_HEADER.size)
            at = RELAY_HEADER.size + 10
            items = []
            for _ in range(n * (count if kind == R_FRAMES else 1)):
                _, length = struct.unpack_from("<BH", body, at)
                items.append(body[at + 3:at + 3 + length])
                at += 3 + length
            if kind == R_FRAMES and n:
                assert first == 0 and items[:count] == [bytes([s]) * 4 for s in range(count)]
                got_frame = True
            elif kind == R_MESSAGES:
                messages.update(items)
        expected = {b"ready%d" % s for s in range(RELAY_SLOTS) if s != slot}
        assert got_frame and messages == expected, (slot, got_frame, messages)
    print("relay frames and messages ok")

    # The host starts the game: it closes, so Delta waits for a new one.
    a.tcp.sendall(lobby_frame(b"gsta", {}))
    started = c.frames(1)
    assert any(cmd == b"+ses" for cmd, _ in started), "no +ses"
    assert not any(cmd == b"+agmugam" for cmd, _ in started), \
        "in-match player got +agmugam (OSDK_GAME_UNAVAILABLE) on start"
    d.search()
    assert d.last_mgm(1.5) is None, "joined a started game"
    print("started game is closed to new players")

    # A gamertag already signed in (in any case) gets a number appended.
    e = Player("alpha")
    names = [f.get("NAME") for cmd, f in e.frames(1) if cmd in (b"auth", b"pers")]
    assert names == ["alpha2", "alpha2"], names
    print("duplicate gamertag renamed")

    # Created session: India hosts it and Delta's waiting team play search
    # joins it; Juliett finds it in the Custom Match list and joins it.
    i, j = Player("India"), Player("Juliett")
    i.frames(0.3)
    i.tcp.sendall(lobby_frame(b"gcre", {"NAME": "India", "SYSFLAGS": 0, "MAXSIZE": 2}))
    created = [f for cmd, f in i.frames(1.5) if cmd == b"gcre"]
    assert created and created[0]["HOST"] == "India", created
    game_id = created[0]["IDENT"]
    mgm = d.last_mgm()
    assert mgm and mgm["IDENT"] == game_id and mgm["COUNT"] == "2", mgm
    j.frames(0.3)
    j.tcp.sendall(lobby_frame(b"gsea", {"COUNT": 20, "SYSFLAGS": 0}))
    listed = j.frames(1)
    rows = [f for cmd, f in listed if cmd == b"+gam"]
    assert any(cmd == b"gsea" for cmd, _ in listed), listed
    assert [r["IDENT"] for r in rows] == [game_id] and rows[0]["COUNT"] == "2", rows
    j.tcp.sendall(lobby_frame(b"gjoi", {"IDENT": game_id}))
    mgm = i.last_mgm()
    assert mgm and mgm["COUNT"] == "3" and mgm["OPPO2"] == "Juliett", mgm
    print("created session listed and joined")

    j.tcp.sendall(lobby_frame(b"gjoi", {"IDENT": 999999}))
    assert any(cmd == b"gjoiugam" for cmd, _ in j.frames(1)), "joined an unknown game"
    i.tcp.sendall(lobby_frame(b"glea", {}))
    i.frames(0.5)
    j.tcp.sendall(lobby_frame(b"gsea", {"COUNT": 20}))
    assert not [f for cmd, f in j.frames(1) if cmd == b"+gam"], "listed a game without a host"
    print("a session closes when its host leaves")

    # Roster packs: a stock game (roster 0, or none sent) never meets a pack.
    f, g, h = Player("Foxtrot", 0x1234ABCD), Player("Golf", 0), Player("Hotel", 0x1234ABCD)
    f.search(ranked=True)
    g.search(ranked=True)
    assert f.last_mgm(1.5) is None, "stock matched a roster pack"
    h.search(ranked=True)
    mgm = f.last_mgm()
    assert mgm and {mgm["OPPO0"], mgm["OPPO1"]} == {"Foxtrot", "Hotel"}, mgm
    print("roster packs only match the same pack")

    # Invites: Kilo invites Lima by chat before creating a session; Lima is
    # told once it exists, and Lima's search lists only that game (public).
    k, l, m = Player("Kilo"), Player("Lima"), Player("Mike")
    for p in (k, l, m):
        p.enter_room()
    m.tcp.sendall(lobby_frame(b"gcre", {"NAME": "Mike", "SYSFLAGS": 0, "MAXSIZE": 2}))
    for p in (k, l, m):
        p.frames(0.3)
    # The game quotes TEXT when it has spaces (DirtySDK); that used to leave
    # the leading quote on the command so "/invite" never matched.
    k.tcp.sendall(lobby_frame(b"mesg", {"TEXT": '"/invite lima"', "ATTR": "C"}))
    notes = [f for cmd, f in k.frames(1) if cmd == b"+msg"]
    assert notes and "WILL BE INVITED" in notes[0]["T"], notes
    assert not [f for cmd, f in l.frames(0.3) if cmd == b"+msg"], "invited before a game"
    k.tcp.sendall(lobby_frame(b"gcre", {"NAME": "Kilo", "SYSFLAGS": 0, "MAXSIZE": 2,
                                        "PRIV": 1}))
    kilo_gcre = [f for cmd, f in k.frames(1.5) if cmd == b"gcre"][0]
    kilo_game = kilo_gcre["IDENT"]
    assert kilo_gcre.get("PRIV", "0") in ("0", 0, ""), kilo_gcre
    assert not (int(kilo_gcre.get("SYSFLAGS", "0") or 0) & (1 << 16)), kilo_gcre
    notes = [f for cmd, f in l.frames(1) if cmd == b"+msg"]
    assert notes and notes[0]["N"] == "Kilo" and "INVITED YOU" in notes[0]["T"], notes
    assert "P" not in notes[0].get("F", ""), notes[0]  # must be public chat
    l.tcp.sendall(lobby_frame(b"gsea", {"COUNT": 20, "SYSFLAGS": 0}))
    rows = [f for cmd, f in l.frames(1) if cmd == b"+gam"]
    assert [r["IDENT"] for r in rows] == [kilo_game] and rows[0]["PRIV"] == "0", rows
    assert not (int(rows[0].get("SYSFLAGS", "0") or 0) & (1 << 16)), rows[0]
    l.tcp.sendall(lobby_frame(b"gjoi", {"IDENT": kilo_game}))
    mgm = k.last_mgm()
    assert mgm and mgm["COUNT"] == "2", mgm
    print("invited player is told and finds the game")

    # Starting a listed session must not send +agmugam to its players (that
    # was OSDK_GAME_UNAVAILABLE mid-start); a room spectator still sees ugam.
    for p in (k, l, m):
        p.frames(0.2)
    k.tcp.sendall(lobby_frame(b"gsta", {}))
    host_frames, guest_frames, spectator = k.frames(1), l.frames(1), m.frames(1)
    assert any(cmd == b"+ses" for cmd, _ in host_frames + guest_frames), "no +ses"
    assert not any(cmd == b"+agmugam" for cmd, _ in host_frames + guest_frames), \
        "match players got +agmugam on gsta"
    assert any(cmd == b"+agmugam" and f.get("IDENT") == kilo_game
               for cmd, f in spectator), "spectator did not see session unlist"
    print("started session does not ugam its own players")
    print("PASS")


if __name__ == "__main__":
    main()
