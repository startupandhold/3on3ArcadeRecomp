#!/usr/bin/env python3
"""Exercises a running server the way game instances would: tunnel
registration and forwarding, ranked and team play quick matches (a team play
game opens with two players, a third joins it, and starting it closes it),
and lockstep relay frames.

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
    def __init__(self, name):
        self.name = name
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.settimeout(2)
        self.udp.sendto(HEADER.pack(TUNNEL_MAGIC, K_HELLO) + struct.pack("<II", 1234, 0),
                        (HOST, 3670))
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
                out.append((self.buffer[:4], lobby_fields(self.buffer[12:length].split(b"\0")[0])))
                self.buffer = self.buffer[length:]
        return out

    def last_mgm(self, timeout=2.5):
        mgm = [f for c, f in self.frames(timeout) if c == b"+mgm"]
        return mgm[-1] if mgm else None

    def search(self, ranked=False):
        self.frames(0.3)
        self.tcp.sendall(lobby_frame(b"gqwk", {
            "MODE": 2, "SYSFLAGS": RANKED_SYSFLAG if ranked else 0}))


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
    assert any(cmd == b"+ses" for cmd, _ in c.frames(1)), "no +ses"
    d.search()
    assert d.last_mgm(1.5) is None, "joined a started game"
    print("started game is closed to new players")
    print("PASS")


if __name__ == "__main__":
    main()
