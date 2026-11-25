#!/usr/bin/env python3
"""Offline tests for the tunnel, relay host and lobby (no sockets: packets go
straight to datagram_received and replies land in a fake transport).

    python -m unittest server/test_server.py     (or: cd server; python -m unittest)
"""

import os
import struct
import sys
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lobby as lobby_mod  # noqa: E402
import tunnel as t  # noqa: E402
from lobby import Client, Game, Lobby  # noqa: E402


class Clock:
    def __init__(self):
        self.now = 1000.0

    def __call__(self):
        return self.now


class FakeTransport:
    def __init__(self):
        self.sent = []
        self.buffered = 0
        self.aborted = False

    def sendto(self, data, addr):
        self.sent.append((bytes(data), addr))

    def get_write_buffer_size(self):
        return self.buffered

    def abort(self):
        self.aborted = True


class FakeWriter:
    def __init__(self):
        self.transport = FakeTransport()
        self.data = []

    def is_closing(self):
        return self.transport.aborted

    def write(self, data):
        self.data.append(data)


def hello(nonce=1, wanted=0, *extra):
    return t.HEADER.pack(t.TUNNEL_MAGIC, t.K_HELLO) + struct.pack(
        f"<{2 + len(extra)}I", nonce, wanted, *extra)


def keepalive(vip, *extra):
    return t.HEADER.pack(t.TUNNEL_MAGIC, t.K_KEEPALIVE) + struct.pack(
        f"<{1 + len(extra)}I", vip, *extra)


def relay(kind, slot, count, a, b, items):
    out = bytearray(t.RELAY_HEADER.pack(t.RELAY_MAGIC, kind, slot, count, 0))
    out += struct.pack("<IIH", a, b, len(items))
    for k, d in items:
        out += struct.pack("<BH", k, len(d)) + d
    return t.HEADER.pack(t.TUNNEL_MAGIC, t.K_RELAY) + bytes(out)


def hash_packet(slot, count, frame, value):
    return (t.HEADER.pack(t.TUNNEL_MAGIC, t.K_RELAY)
            + t.RELAY_HEADER.pack(t.RELAY_MAGIC, t.R_HASH, slot, count, 0)
            + struct.pack("<II", frame, value))


def parse_frames(data):
    """R_FRAMES packet sent by the server -> (first, [frame items])."""
    body = data[t.HEADER.size:]
    _, kind, _, count, _ = t.RELAY_HEADER.unpack_from(body)
    if kind != t.R_FRAMES:
        return None
    _, first, n = struct.unpack_from("<IIH", body, t.RELAY_HEADER.size)
    at = t.RELAY_HEADER.size + 10
    frames = []
    for _ in range(n):
        frame = []
        for _ in range(count):
            k, length = struct.unpack_from("<BH", body, at)
            frame.append((k, body[at + 3:at + 3 + length]))
            at += 3 + length
        frames.append(frame)
    return first, frames


class Base(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        patcher = mock.patch.object(time, "monotonic", self.clock)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.lobby = Lobby("127.0.0.1")
        self.lobby.notify_later = lambda *a, **k: None
        self.tunnel = t.Tunnel(self.lobby)
        self.out = FakeTransport()
        self.tunnel.connection_made(self.out)

    def recv(self, data, addr):
        self.tunnel.datagram_received(data, addr)

    def replies(self, addr, kind):
        got = [d for d, a in self.out.sent if a == addr
               and t.HEADER.unpack_from(d)[1] == kind]
        return got

    def register(self, addr, version=2, token=0):
        self.recv(hello(7, 0, 0, version, token), addr)
        welcome = self.replies(addr, t.K_WELCOME)[-1]
        nonce, vip, token, server = struct.unpack_from("<IIII", welcome, t.HEADER.size)
        return vip, token

    def client(self, ip, peer_ip="1.1.1.1"):
        c = Client(self.lobby, FakeWriter(), peer_ip)
        self.lobby.clients.add(c)
        c.mac_ip = ip
        self.lobby.reindex(c, None)
        return c

    def team(self, n=3):
        """`n` registered players in one lobby game -> list of (addr, vip)."""
        players = [(("1.2.3.%d" % (i + 1), 5000 + i), None) for i in range(n)]
        players = [(addr, self.register(addr)[0]) for addr, _ in players]
        self.clients = [self.client(t.format_ip(vip)) for _, vip in players]
        self.game = Game({}, self.clients, ranked=False)
        return players


class TunnelTests(Base):
    def test_v1_hello_and_keepalive(self):
        a = ("2.2.2.2", 1)
        self.recv(hello(5, 0), a)  # v1: no roster
        welcome = self.replies(a, t.K_WELCOME)[-1]
        nonce, vip = struct.unpack_from("<II", welcome, t.HEADER.size)
        self.assertEqual(nonce, 5)
        self.assertEqual(self.tunnel.by_vip[vip].token, 0)
        b = ("2.2.2.3", 1)
        self.recv(hello(6, 0, 0x1234), b)  # v1 with roster
        vip_b = struct.unpack_from("<II", self.replies(b, t.K_WELCOME)[-1], t.HEADER.size)[1]
        self.assertEqual(self.lobby.rosters[vip_b], 0x1234)
        self.recv(keepalive(vip), a)
        self.recv(keepalive(vip, 0), a)
        self.assertEqual(self.replies(a, t.K_KEEPALIVE_ACK), [])
        # A v1 keepalive re-registers a vip after a server restart.
        c = ("2.2.2.4", 1)
        self.recv(keepalive(t.VIP_NET | 0x0505), c)
        self.assertEqual(self.tunnel.by_addr[c].vip, t.VIP_NET | 0x0505)

    def test_v2_welcome_and_keepalive_ack(self):
        a = ("3.3.3.3", 9)
        vip, token = self.register(a)
        self.assertNotEqual(token, 0)
        server = struct.unpack_from("<IIII", self.replies(a, t.K_WELCOME)[-1],
                                    t.HEADER.size)[3]
        self.assertEqual(server, t.PROTOCOL_VERSION)
        self.recv(keepalive(vip, 0, 2, token, 77, 42), a)
        ack = self.replies(a, t.K_KEEPALIVE_ACK)[-1]
        self.assertEqual(struct.unpack_from("<II", ack, t.HEADER.size), (77, vip))
        self.assertEqual(self.tunnel.by_vip[vip].rtt_ms, 42)
        self.assertEqual(self.lobby.rtts[vip], 42)
        self.assertEqual(self.lobby.versions[vip], 2)

    def test_token_protected_rebind(self):
        a, b = ("4.4.4.4", 1), ("5.5.5.5", 2)
        vip, token = self.register(a)
        self.clock.now += t.REBIND_AFTER + 1
        self.recv(keepalive(vip, 0, 2, token ^ 1, 1, 0), b)
        self.assertNotIn(b, self.tunnel.by_addr)
        self.recv(hello(1, vip, 0, 2, token ^ 1), b)
        self.assertNotEqual(self.tunnel.by_addr[b].vip, vip)
        c = ("6.6.6.6", 3)
        self.recv(keepalive(vip, 0, 2, token, 1, 0), c)
        self.assertEqual(self.tunnel.by_addr[c].vip, vip)
        self.assertEqual(self.tunnel.by_vip[vip].token, token)
        # Not quiet any more: even the right token waits.
        self.recv(hello(1, vip, 0, 2, token), ("7.7.7.7", 4))
        self.assertNotEqual(self.tunnel.by_addr[("7.7.7.7", 4)].vip, vip)

    def test_registration_rate_limit(self):
        for port in range(30):
            self.recv(hello(1, 0), ("8.8.8.8", port))
        self.assertEqual(len(self.tunnel.by_vip), t.REGISTER_IP_BURST)
        for i in range(300):
            self.recv(hello(1, 0), ("9.9.%d.%d" % (i // 250, i % 250), 1))
        self.assertEqual(len(self.tunnel.by_vip), t.REGISTER_BURST)
        self.assertGreater(self.tunnel.rejected, 200)
        self.clock.now += 1
        self.recv(hello(1, 0), ("10.0.0.1", 1))
        self.assertEqual(len(self.tunnel.by_vip), t.REGISTER_BURST + 1)

    def test_timeout_prunes_and_drops_lobby_client(self):
        a = ("4.4.4.4", 1)
        vip, _ = self.register(a)
        c = self.client(t.format_ip(vip))
        self.clock.now += t.PEER_TIMEOUT + 1
        self.tunnel.sweep(self.clock.now)
        self.assertNotIn(vip, self.lobby.rosters)
        self.assertNotIn(vip, self.lobby.versions)
        self.assertTrue(c.writer.transport.aborted)


class RelayTests(Base):
    def test_frame_build_three_slots(self):
        players = self.team()
        for slot, (addr, _) in enumerate(players):
            self.recv(relay(t.R_INPUTS, slot, 3, 0, 0, [(2, bytes([slot]) * 4)]), addr)
        for addr, _ in players:
            frames = [parse_frames(d) for d in self.replies(addr, t.K_RELAY)]
            frames = [f for f in frames if f and f[1]]
            self.assertTrue(frames)
            self.assertEqual(frames[-1][1][0], [(2, bytes([s]) * 4) for s in range(3)])

    def test_oversized_input_dropped(self):
        players = self.team()
        self.recv(relay(t.R_INPUTS, 0, 3, 0, 0, [(2, b"x" * 253)]), players[0][0])
        self.assertEqual(self.tunnel.matches, {})
        self.recv(relay(t.R_MESSAGES, 0, 3, 0, 0, [(2, b"x" * 1201)]), players[0][0])
        self.assertEqual(self.tunnel.matches, {})
        self.recv(relay(t.R_INPUTS, 0, 3, 0, 0, [(2, b"x" * 252)]), players[0][0])
        match = self.tunnel.matches[self.game.id]
        self.assertEqual(match.slots[0].inputs_received, 1)

    def test_dead_slot_filled(self):
        players = self.team()
        for slot, (addr, _) in enumerate(players):
            self.recv(relay(t.R_INPUTS, slot, 3, 0, 0, [(2, b"a")]), addr)
        match = self.tunnel.matches[self.game.id]
        self.assertEqual(match.frames_end, 1)
        self.clock.now += t.SLOT_DEAD + 1
        for slot in (0, 1):
            self.recv(relay(t.R_INPUTS, slot, 3, 1, 1, [(2, b"b")]), players[slot][0])
        with self.assertLogs("tunnel", "WARNING") as logs:
            self.tunnel.tick(self.clock.now)
        self.assertIn("slot 2 (%s) dropped after 9s" % t.format_ip(players[2][1]),
                      logs.output[0])
        self.assertTrue(match.slots[2].dead)
        self.assertEqual(match.frames_end, 2)
        self.assertEqual(match.frames[-1], [(2, b"b"), (2, b"b"), t.NO_INPUT])
        # Its packets are ignored, and it no longer holds back frame trimming.
        self.recv(relay(t.R_INPUTS, 2, 3, 1, 1, [(2, b"c")]), players[2][0])
        self.assertEqual(match.slots[2].inputs_received, 1)
        for slot in (0, 1):
            self.recv(relay(t.R_INPUTS, slot, 3, 2, 2, [(2, b"c")]), players[slot][0])
        self.assertEqual(match.frames_base, 2)
        # Everyone gone: the match ends.
        self.clock.now += t.SLOT_DEAD + 1
        with self.assertLogs("tunnel", "INFO"):
            self.tunnel.tick(self.clock.now)
        self.assertEqual(self.tunnel.matches, {})
        self.assertEqual(self.tunnel.vip_match, {})
        self.assertEqual(self.tunnel.dead_slots, 1)  # the rest just ended

    def test_never_heard_slot_dies(self):
        players = self.team()
        for slot in (0, 1):
            self.recv(relay(t.R_INPUTS, slot, 3, 0, 0, [(2, b"a")]), players[slot][0])
        match = self.tunnel.matches[self.game.id]
        for _ in range(int(t.SLOT_FIRST / 5) + 1):
            self.clock.now += 5
            for slot in (0, 1):
                self.recv(relay(t.R_MESSAGES, slot, 3, 0, 1, []), players[slot][0])
            self.tunnel.tick(self.clock.now)
        self.assertTrue(match.slots[2].dead)
        self.assertFalse(match.slots[0].dead)
        self.assertEqual(match.frames_end, 1)

    def test_slot_spoof_rejected(self):
        players = self.team()
        self.recv(relay(t.R_INPUTS, 0, 3, 0, 0, [(2, b"a")]), players[0][0])
        match = self.tunnel.matches[self.game.id]
        with self.assertLogs("tunnel", "WARNING"):
            self.recv(relay(t.R_INPUTS, 0, 3, 0, 1, [(2, b"b")]), players[1][0])
        self.assertEqual(match.slots[0].inputs_received, 1)
        self.assertEqual(match.slots[0].vip, players[0][1])
        # A vip bound to one slot can't take another.
        self.recv(relay(t.R_INPUTS, 2, 3, 0, 0, [(2, b"b")]), players[0][0])
        self.assertEqual(match.slots[2].vip, 0)

    def test_input_ahead_cap(self):
        players = self.team()
        items = [(2, b"i")] * 20
        self.recv(relay(t.R_INPUTS, 0, 3, 0, 0, items), players[0][0])
        match = self.tunnel.matches[self.game.id]
        self.assertEqual(match.slots[0].inputs_received, t.INPUT_AHEAD)
        self.assertEqual(len(match.slots[0].pending), t.INPUT_AHEAD)

    def test_message_queue_cap(self):
        players = self.team()
        self.recv(relay(t.R_INPUTS, 1, 3, 0, 0, [(2, b"a")]), players[1][0])
        sent = 0
        with self.assertLogs("tunnel", "WARNING") as logs:
            while sent <= t.MAX_QUEUE:
                self.recv(relay(t.R_MESSAGES, 0, 3, 0, sent, [(7, b"m")] * 50), players[0][0])
                sent += 50
        match = self.tunnel.matches[self.game.id]
        self.assertTrue(match.slots[1].dead)
        self.assertTrue(match.slots[2].dead)
        self.assertTrue(any("messages unacknowledged" in line for line in logs.output))

    def test_hash_mismatch_logged(self):
        players = self.team()
        for slot, (addr, _) in enumerate(players):
            self.recv(relay(t.R_INPUTS, slot, 3, 0, 0, [(2, b"a")]), addr)
        for slot, (addr, _) in enumerate(players):
            self.recv(hash_packet(slot, 3, 60, 0xAAAA), addr)
        self.assertEqual(self.tunnel.desyncs, 0)
        self.recv(hash_packet(0, 3, 120, 0xBBBB), players[0][0])
        self.recv(hash_packet(1, 3, 120, 0xBBBB), players[1][0])
        with self.assertLogs("tunnel", "WARNING") as logs:
            self.recv(hash_packet(2, 3, 120, 0xCCCC), players[2][0])
        self.assertIn("desync at frame 120", logs.output[0])
        self.assertIn("0000cccc", logs.output[0])
        self.assertEqual(self.tunnel.desyncs, 1)
        for frame in range(1000):  # bounded, and logged only once
            self.recv(hash_packet(0, 3, 180 + frame, frame), players[0][0])
        self.assertLessEqual(len(self.tunnel.matches[self.game.id].hashes), t.HASH_FRAMES)
        self.assertEqual(self.tunnel.desyncs, 1)

    def test_one_match_per_game(self):
        players = self.team(4)
        self.recv(relay(t.R_INPUTS, 0, 3, 0, 0, [(2, b"a")]), players[0][0])
        self.recv(relay(t.R_INPUTS, 3, 4, 0, 0, [(2, b"a")]), players[3][0])
        self.recv(relay(t.R_INPUTS, 1, 4, 0, 0, [(2, b"a")]), players[1][0])
        self.assertEqual(list(self.tunnel.matches), [self.game.id])
        self.assertEqual(self.tunnel.matches[self.game.id].count, 3)

    def test_match_survives_lobby_disconnect(self):
        players = self.team()
        for slot, (addr, _) in enumerate(players):
            self.recv(relay(t.R_INPUTS, slot, 3, 0, 0, [(2, b"a")]), addr)
        for c in self.clients:  # every lobby connection drops
            self.lobby.leave_game(c)
            self.lobby.clients.discard(c)
            self.lobby.reindex(c, c.ip)
        self.assertIsNone(self.lobby.game_of_ip(t.format_ip(players[0][1])))
        for slot, (addr, _) in enumerate(players):
            self.recv(relay(t.R_INPUTS, slot, 3, 1, 1, [(2, b"b")]), addr)
        self.assertEqual(self.tunnel.matches[self.game.id].frames_end, 2)


class LobbyTests(Base):
    def test_tunnel_address_must_match_peer_ip(self):
        vip, _ = self.register(("1.2.3.4", 5))
        mac = "$0022" + "".join(f"{b:02x}" for b in struct.pack(">I", vip))
        liar = Client(self.lobby, FakeWriter(), "6.6.6.6")
        self.lobby.clients.add(liar)
        liar.learn({"MAC": mac})
        self.assertIsNone(liar.mac_ip)
        owner = Client(self.lobby, FakeWriter(), "1.2.3.4")
        self.lobby.clients.add(owner)
        owner.learn({"MAC": mac})
        self.assertEqual(owner.mac_ip, t.format_ip(vip))
        self.assertIn(owner, self.lobby.by_ip[t.format_ip(vip)])
        direct = Client(self.lobby, FakeWriter(), "6.6.6.6")
        direct.learn({"MAC": "$0022c0a80105"})  # 192.168.1.5: direct mode
        self.assertEqual(direct.mac_ip, "192.168.1.5")

    def test_ping_aware_pairing(self):
        a, b, c = (self.client("10.66.0.%d" % i) for i in (1, 2, 3))
        for x, rtt in ((a, 200), (b, 150), (c, 40)):
            self.lobby.rtts[x.vip] = rtt
        self.lobby.quick_match(a, {"MODE": "2", "SYSFLAGS": str(lobby_mod.RANKED_SYSFLAG)})
        self.lobby.quick_match(b, {"MODE": "2", "SYSFLAGS": str(lobby_mod.RANKED_SYSFLAG)})
        self.assertIsNone(a.game)
        self.lobby.quick_match(c, {"MODE": "2", "SYSFLAGS": str(lobby_mod.RANKED_SYSFLAG)})
        self.assertIs(a.game, c.game)
        self.assertIsNone(b.game)

    def test_pairing_after_wait_and_by_version(self):
        a, b, v1 = (self.client("10.66.0.%d" % i) for i in (1, 2, 3))
        self.lobby.rtts[a.vip], self.lobby.rtts[b.vip] = 200, 150
        self.lobby.versions[a.vip] = self.lobby.versions[b.vip] = 2
        for x in (a, v1, b):
            self.lobby.quick_match(x, {"MODE": "2", "SYSFLAGS": str(lobby_mod.RANKED_SYSFLAG)})
        self.assertIsNone(a.game)
        self.assertIsNone(v1.game)
        self.clock.now += lobby_mod.PAIR_WAIT
        queue = self.lobby.queues[(True, 0, 2)]
        self.assertTrue(self.lobby.pair(True, queue, self.clock.now))
        self.assertIs(a.game, b.game)
        self.assertIsNone(v1.game)

    def test_guest_cap_and_chat_limit(self):
        players = [self.client("10.66.0.%d" % i) for i in (1, 2)]
        game = Game({}, players, ranked=False)
        self.lobby.reply(b"gset", {"GUEST": "A(1),A(2),A(3)"}, players[0])
        self.lobby.reply(b"gset", {"GUEST": "B(1),B(2),B(3),B(4)"}, players[1])
        self.assertEqual(sum(p.users for p in game.players), 5)
        sent = []
        for i in range(10):
            self.lobby.chat(players[0], {"TEXT": "hi%d" % i})
        sent = [d for d in players[1].writer.data if d.startswith(b"+msg")]
        self.assertEqual(len(sent), lobby_mod.CHAT_LIMIT)

    def test_backpressure_drops_client(self):
        c = self.client("10.66.0.1")
        c.writer.transport.buffered = lobby_mod.SEND_BUFFER_LIMIT + 1
        c.send(b"x")
        self.assertTrue(c.writer.transport.aborted)
        self.assertEqual(c.writer.data, [])

    def test_proxy_protocol_line(self):
        import asyncio

        class StreamWriter(FakeWriter):
            def get_extra_info(self, name):
                return ("172.16.0.1", 4000) if name == "peername" else None

            async def drain(self):
                pass

            def close(self):
                pass

        async def run(trust):
            self.lobby.trust_proxy = trust
            reader = asyncio.StreamReader()
            reader.feed_data(b"PROXY TCP4 1.2.3.4 5.6.7.8 1111 10901\r\n"
                             + lobby_mod.lobby_frame(b"auth", {"GTAG": "Proxy"}))
            reader.feed_eof()
            writer = StreamWriter()
            await self.lobby.handle(reader, writer)
            return b"".join(writer.data)

        self.assertIn(b"ADDR=1.2.3.4", asyncio.run(run(True)))
        # Without a proxy the line is just a bad frame: the connection drops.
        self.assertNotIn(b"ADDR=1.2.3.4", asyncio.run(run(False)))
        self.assertEqual(self.lobby.clients, set())
        self.assertEqual(self.lobby.by_ip, {})

    def test_non_host_gsta_not_broadcast(self):
        players = [self.client("10.66.0.%d" % i) for i in (1, 2, 3)]
        Game({}, players, ranked=False)
        self.lobby.reply(b"gsta", {}, players[1])
        self.assertFalse([d for d in players[0].writer.data if d.startswith(b"+ses")])
        self.assertTrue([d for d in players[1].writer.data if d.startswith(b"+ses")])
        self.lobby.reply(b"gsta", {}, players[0])
        self.assertTrue([d for d in players[2].writer.data if d.startswith(b"+ses")])


if __name__ == "__main__":
    unittest.main()
