#!/usr/bin/env python3
"""Exercises a running server the way game instances would: tunnel
registration and forwarding, lobby sign-in and a quick match, and (for team
play) lockstep relay frames. Pass the server's match size:

    python selftest.py [host] [match size, default 2]
"""

import socket
import struct
import sys
import time

from lobby import lobby_frame
from tunnel import (HEADER, K_DATA, K_HELLO, K_RELAY, K_WELCOME, RELAY_HEADER, RELAY_MAGIC,
                    R_FRAMES, R_INPUTS, R_MESSAGES, TUNNEL_MAGIC, format_ip, parse_ip)

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 2


def udp_client():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    s.sendto(HEADER.pack(TUNNEL_MAGIC, K_HELLO) + struct.pack("<II", 1234, 0), (HOST, 3670))
    data = s.recv(2048)
    magic, kind = HEADER.unpack_from(data)
    nonce, vip = struct.unpack_from("<II", data, HEADER.size)
    assert magic == TUNNEL_MAGIC and kind == K_WELCOME and nonce == 1234, data
    return s, vip


def lobby_client(name, vip):
    t = socket.create_connection((HOST, 10901))
    t.settimeout(3)
    mac = "$0022" + "".join(f"{b:02x}" for b in struct.pack(">I", vip))
    t.sendall(lobby_frame(b"auth", {"GTAG": name, "MAC": mac}))
    t.sendall(lobby_frame(b"pers", {"PERS": name, "MAC": mac}))
    return t


def recv_until(sock, needle, timeout=3):
    sock.settimeout(timeout)
    data = b""
    end = time.time() + timeout
    while needle not in data and time.time() < end:
        data += sock.recv(65536)
    return needle in data


def relay_packet(kind, slot, a, b, items):
    out = bytearray(RELAY_HEADER.pack(RELAY_MAGIC, kind, slot, SIZE, 0))
    out += struct.pack("<IIH", a, b, len(items))
    for k, d in items:
        out += struct.pack("<BH", k, len(d)) + d
    return HEADER.pack(TUNNEL_MAGIC, K_RELAY) + bytes(out)


def main():
    players = []
    for i in range(SIZE):
        u, vip = udp_client()
        print(f"player {i}: virtual address {format_ip(vip)}")
        players.append((u, vip, lobby_client(f"Test{i}", vip)))

    # Tunnel: player 0 -> player 1, port 3659.
    u0, vip0, _ = players[0]
    u1, vip1, _ = players[1]
    u0.sendto(HEADER.pack(TUNNEL_MAGIC, K_DATA) + struct.pack("<IHH", vip1, 3659, 3659)
              + b"hello", (HOST, 3670))
    data = u1.recv(2048)
    src, sport, dport = struct.unpack_from("<IHH", data, HEADER.size)
    assert src == vip0 and dport == 3659 and data[HEADER.size + 8:] == b"hello", data
    print("tunnel forwarding ok")

    # Lobby: everyone searches; the server pairs them in one game.
    for _, _, t in players:
        t.sendall(lobby_frame(b"gqwk", {"MODE": 2, "SYSFLAGS": 0}))
    for i, (_, _, t) in enumerate(players):
        assert recv_until(t, b"+mgm"), f"player {i} got no +mgm"
    print(f"{SIZE}-player match made")
    if SIZE < 3:
        print("PASS")
        return

    # Relay: each slot sends input 0 and a control message; every slot gets
    # frame 0 holding every slot's input, and the other slots' messages.
    for slot, (u, _, _) in enumerate(players):
        u.sendto(relay_packet(R_INPUTS, slot, 0, 0, [(2, bytes([slot]) * 4)]), (HOST, 3670))
        u.sendto(relay_packet(R_MESSAGES, slot, 0, 0, [(7, b"ready%d" % slot)]), (HOST, 3670))
    for slot, (u, _, _) in enumerate(players):
        got_frame, messages = False, set()
        end = time.time() + 2
        while (not got_frame or len(messages) < SIZE - 1) and time.time() < end:
            data = u.recv(2048)
            body = data[HEADER.size:]
            _, kind, _, count, _ = RELAY_HEADER.unpack_from(body)
            _, first, n = struct.unpack_from("<IIH", body, RELAY_HEADER.size)
            at = RELAY_HEADER.size + 10
            if kind == R_FRAMES and n:
                inputs = []
                for _ in range(n * count):
                    k, length = struct.unpack_from("<BH", body, at)
                    inputs.append(body[at + 3:at + 3 + length])
                    at += 3 + length
                assert first == 0 and inputs == [bytes([s]) * 4 for s in range(SIZE)], inputs
                got_frame = True
            elif kind == R_MESSAGES:
                for _ in range(n):
                    k, length = struct.unpack_from("<BH", body, at)
                    messages.add(body[at + 3:at + 3 + length])
                    at += 3 + length
        expected = {b"ready%d" % s for s in range(SIZE) if s != slot}
        assert got_frame and messages == expected, (slot, got_frame, messages)
    print("relay frames and messages ok")
    print("PASS")


if __name__ == "__main__":
    main()
