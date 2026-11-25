"""UDP tunnel and lockstep relay host.

Every game registers with the tunnel and gets a virtual IPv4 address
(10.66.x.y) that it advertises as its own. Peer-to-peer datagrams to another
virtual address go to the server, which forwards them to that player, so
players never need open ports: each one only talks to the server.

Team play's lockstep relay (see src/nhl3on3_lockstep_relay.cpp) also runs
here: every console is a relay client and the server is the host, building
frame N once it holds input N from every live slot.

Tunnel packets (little-endian), header: u32 magic, u8 kind, 3 reserved bytes.
Fields in [] are optional (v2, see PROTOCOL_V2.md); missing ones read as 0.
  kHello      c->s  u32 nonce, u32 wanted virtual address (0 = any)
                    [, u32 roster, u32 version, u32 token]
  kWelcome    s->c  u32 nonce, u32 virtual address, u32 token, u32 version
  kKeepalive  c->s  u32 virtual address[, u32 roster, u32 version, u32 token,
                    u32 seq, u32 rtt_ms]; also re-registers it after a server
                    restart or a NAT mapping change
  kKeepaliveAck s->c  u32 seq, u32 virtual address (to keepalives with a seq)
  kData       c->s  u32 dest address, u16 dest port, u16 source port, payload
              s->c  u32 source address, u16 source port, u16 dest port, payload
  kRelay      both  one lockstep relay packet
The roster is the game's roster pack id (0 = stock) and version its protocol
version (0 = v1); quick match only pairs equal (roster, version). The token
is the server's random secret for a virtual address (0 for v1 clients): a
quiet address only moves to a new endpoint that presents it.

Relay slots bind to the first virtual address that sends for them. A slot
silent for SLOT_DEAD seconds (or never heard within SLOT_FIRST of the start)
is dead: its input is filled with "no input" so the others play on.
"""

import asyncio
import logging
import secrets
import struct
import time
from collections import deque

log = logging.getLogger("tunnel")

PROTOCOL_VERSION = 2
TUNNEL_MAGIC = 0x3154484E  # "NHT1"
K_HELLO, K_WELCOME, K_KEEPALIVE, K_DATA, K_RELAY, K_KEEPALIVE_ACK = 1, 2, 3, 4, 5, 6
HEADER = struct.Struct("<IB3x")
VIP_NET = (10 << 24) | (66 << 16)
PEER_TIMEOUT = 120.0
# A virtual address whose endpoint has been silent this long may move to a new
# endpoint (the same player after a NAT mapping change).
REBIND_AFTER = 15.0
MAX_DATAGRAM = 1500
RATE_PACKETS = 600  # per second per endpoint
RATE_BURST = 1200
# New registrations (and rebinds), so spoofed hellos can't drain the pool.
REGISTER_RATE, REGISTER_BURST = 50, 100  # per second, all sources
REGISTER_IP_RATE, REGISTER_IP_BURST = 5, 10  # per second, per source IP
REGISTER_LOGS = 20  # "registered" lines per minute; the rest are counted

RELAY_MAGIC = 0x3152484E  # "NHR1"
R_INPUTS, R_FRAMES, R_MESSAGES, R_HASH = 1, 2, 3, 4
RELAY_HEADER = struct.Struct("<IBBBB")
MAX_RELAY_PACKET = 1400
MAX_SLOTS = 12
MAX_INPUT = 252
MAX_MESSAGE = 1200
INPUT_AHEAD = 16  # 2 x the client's kWindow
MAX_QUEUE = 256  # unacknowledged messages to one slot
NO_INPUT = (1, b"")
SLOT_DEAD = 8.0
SLOT_FIRST = 30.0
HASH_FRAMES = 32  # frame numbers whose hashes are kept for comparing
RESEND = 0.020
MATCH_IDLE = 30.0
STARTUP_STALL = 10.0  # seconds without a first frame before the slots are logged


def format_ip(ip):
    return f"{ip >> 24}.{(ip >> 16) & 255}.{(ip >> 8) & 255}.{ip & 255}"


def parse_ip(text):
    parts = [int(p) for p in text.split(".")]
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]


def u32s(body, n):
    """The first `n` u32 fields of `body`; missing trailing ones are 0."""
    have = min(n, len(body) // 4)
    return list(struct.unpack_from(f"<{have}I", body)) + [0] * (n - have)


class Bucket:
    """Token bucket: `rate` per second, up to `burst`."""

    def __init__(self, rate, burst, now):
        self.rate = rate
        self.burst = burst
        self.tokens = burst
        self.time = now

    def allow(self, now):
        self.tokens = min(self.burst, self.tokens + (now - self.time) * self.rate)
        self.time = now
        if self.tokens < 1:
            return False
        self.tokens -= 1
        return True


class Peer:
    def __init__(self, vip, addr, token, now):
        self.vip = vip
        self.addr = addr
        self.token = token
        self.version = 0
        self.rtt_ms = 0
        self.last_seen = now
        self.rate = Bucket(RATE_PACKETS, RATE_BURST, now)


# -- lockstep relay host ----------------------------------------------------

def read_input(data, at):
    """u8 type, u16 length, data -> ((type, bytes), next offset) or None."""
    if at + 3 > len(data):
        return None
    kind, length = data[at], data[at + 1] | (data[at + 2] << 8)
    end = at + 3 + length
    if end > len(data):
        return None
    return (kind, bytes(data[at + 3:end])), end


def read_items(data, at, n, limit):
    """`n` items from `at` (fewer if truncated), or None if one is longer than
    `limit`: the whole packet is invalid."""
    items = []
    for _ in range(n):
        got = read_input(data, at)
        if not got:
            break
        item, at = got
        if len(item[1]) > limit:
            return None
        items.append(item)
    return items


def put_input(out, item):
    kind, data = item
    out += struct.pack("<BH", kind, len(data))
    out += data


class Channel:
    """Reliable, ordered control messages to one slot."""

    def __init__(self):
        self.out = deque()
        self.out_base = 0
        self.in_next = 0
        self.ack_sent = -1
        self.last_send = 0.0


class Slot:
    def __init__(self):
        self.vip = 0
        self.messages = Channel()
        self.inputs_received = 0
        self.frames_acked = 0
        self.frames_sent = 0
        self.last_send = 0.0
        self.last_packet = 0.0
        self.pending = deque()
        self.dead = False
        self.spoof_logged = False


class Match:
    def __init__(self, tunnel, key, count):
        self.tunnel = tunnel
        self.key = key
        self.count = count
        self.slots = [Slot() for _ in range(count)]
        self.frames = deque()
        self.frames_base = 0
        self.started = self.last_packet = time.monotonic()
        self.built_logged = 0
        self.stall_logged = False
        self.hashes = {}  # frame number -> {slot: hash}
        self.desync_logged = False

    @property
    def frames_end(self):
        return self.frames_base + len(self.frames)

    def label(self, index):
        vip = self.slots[index].vip
        return f"slot {index} ({format_ip(vip) if vip else '-'})"

    def log_slots(self, why, now):
        """Logs each slot's traffic, to show which console went quiet."""
        log.info("match %s: %s after %.1fs, %d frames built", self.key, why,
                 now - self.started, self.frames_end)
        for index, slot in enumerate(self.slots):
            ch = slot.messages
            quiet = f"{now - slot.last_packet:.1f}s ago" if slot.last_packet else "never"
            log.info("match %s:   %s%s: last packet %s, messages from it %d, "
                     "to it %d (%d unacknowledged), inputs %d, frames acked %d",
                     self.key, self.label(index), " dead" if slot.dead else "", quiet,
                     ch.in_next, ch.out_base + len(ch.out), len(ch.out),
                     slot.inputs_received, slot.frames_acked)

    def header(self, kind):
        return bytearray(RELAY_HEADER.pack(RELAY_MAGIC, kind, 0, self.count, 0))

    def send(self, slot, packet):
        self.tunnel.send_relay(slot.vip, bytes(packet))

    def kill(self, index, why):
        slot = self.slots[index]
        slot.dead = True
        slot.pending.clear()
        slot.messages.out.clear()
        self.tunnel.dead_slots += 1
        log.warning("match %s: %s dropped %s, filling with no input", self.key,
                    self.label(index), why)

    def check_dead(self, now):
        """Marks silent slots dead; True once every slot is dead or silent (the
        match is over: the remaining ones are not counted as dropped)."""
        silent = []
        for index, slot in enumerate(self.slots):
            if slot.dead:
                continue
            if slot.last_packet and now - slot.last_packet > SLOT_DEAD:
                silent.append((index, f"after {now - slot.last_packet:.0f}s"))
            elif not slot.last_packet and now - self.started > SLOT_FIRST:
                silent.append((index, f"after {now - self.started:.0f}s (never heard)"))
        if len(silent) == sum(not s.dead for s in self.slots):
            return True
        for index, why in silent:
            self.kill(index, why)
        return False

    def handle(self, kind, slot_index, vip, data, at, items):
        slot = self.slots[slot_index]
        if not slot.vip:
            if self.tunnel.vip_match.get(vip, (None,))[0] is self:
                return  # already holds another slot of this match
            log.info("match %s: slot %d is %s", self.key, slot_index, format_ip(vip))
            slot.vip = vip
            self.tunnel.vip_match[vip] = (self, slot_index)
        elif slot.vip != vip:
            if not slot.spoof_logged:
                slot.spoof_logged = True
                log.warning("match %s: dropping packets for %s from %s", self.key,
                            self.label(slot_index), format_ip(vip))
            return
        if slot.dead:
            return
        now = time.monotonic()
        self.last_packet = slot.last_packet = now
        if kind == R_HASH:
            if len(data) >= at + 8:
                self.note_hash(slot_index, *struct.unpack_from("<II", data, at))
            return
        if len(data) < at + 10:
            return
        acked, first = struct.unpack_from("<II", data, at)
        if kind == R_INPUTS:
            slot.frames_acked = max(slot.frames_acked, min(acked, self.frames_end))
            for i, item in enumerate(items):
                if first + i != slot.inputs_received:
                    continue
                if slot.inputs_received >= self.frames_end + INPUT_AHEAD:
                    break
                slot.pending.append(item)
                slot.inputs_received += 1
        elif kind == R_MESSAGES:
            ch = slot.messages
            while ch.out_base < acked and ch.out:
                ch.out.popleft()
                ch.out_base += 1
            for i, item in enumerate(items):
                if first + i != ch.in_next:
                    continue
                ch.in_next += 1
                for other_index, other in enumerate(self.slots):
                    if other_index == slot_index or other.dead:
                        continue
                    if len(other.messages.out) >= MAX_QUEUE:
                        self.kill(other_index, f"with {MAX_QUEUE} messages unacknowledged")
                        continue
                    other.messages.out.append(item)
                    other.messages.last_send = 0.0
        self.pump(now)

    def note_hash(self, slot_index, frame, value):
        """Compares the state hashes the live slots report for `frame` and
        logs the first mismatch."""
        if self.desync_logged:
            return
        seen = self.hashes.setdefault(frame, {})
        seen[slot_index] = value
        while len(self.hashes) > HASH_FRAMES:
            del self.hashes[next(iter(self.hashes))]
        live = [i for i, s in enumerate(self.slots) if not s.dead]
        if frame not in self.hashes or any(i not in seen for i in live):
            return
        del self.hashes[frame]
        if len({seen[i] for i in live}) > 1:
            self.desync_logged = True
            self.tunnel.desyncs += 1
            log.warning("match %s: desync at frame %d: %s", self.key, frame,
                        ", ".join(f"{self.label(i)} {h:08x}" for i, h in sorted(seen.items())))

    def build_frames(self):
        while all(s.dead or s.pending for s in self.slots):
            self.frames.append([NO_INPUT if s.dead else s.pending.popleft()
                                for s in self.slots])
            end = self.frames_end
            if end == 1 or end - self.built_logged >= 3600:
                self.built_logged = end
                log.info("match %s: built frame %d", self.key, end)

    def send_frames(self, slot, now):
        first = max(slot.frames_acked, self.frames_base)
        packet = self.header(R_FRAMES)
        packet += struct.pack("<II", slot.inputs_received, first)
        count_at = len(packet)
        packet += b"\0\0"
        n = 0
        for f in range(first, self.frames_end):
            frame = self.frames[f - self.frames_base]
            size = sum(3 + len(d) for _, d in frame)
            if n and len(packet) + size > MAX_RELAY_PACKET:
                break
            for item in frame:
                put_input(packet, item)
            n += 1
        struct.pack_into("<H", packet, count_at, n)
        self.send(slot, packet)
        slot.frames_sent = first + n
        slot.last_send = now

    def send_channel(self, slot, now):
        ch = slot.messages
        packet = self.header(R_MESSAGES)
        packet += struct.pack("<II", ch.in_next, ch.out_base)
        count_at = len(packet)
        packet += b"\0\0"
        n = 0
        for item in ch.out:
            if n and len(packet) + 3 + len(item[1]) > MAX_RELAY_PACKET:
                break
            put_input(packet, item)
            n += 1
        struct.pack_into("<H", packet, count_at, n)
        self.send(slot, packet)
        ch.ack_sent = ch.in_next
        ch.last_send = now

    def pump(self, now):
        if all(s.dead for s in self.slots):
            return
        self.build_frames()
        end = self.frames_end
        for slot in self.slots:
            if not slot.vip or slot.dead:
                continue
            fresh = slot.frames_sent < end
            stale = slot.frames_acked < end and now - slot.last_send >= RESEND
            if fresh or stale or now - slot.last_send >= RESEND * 10:
                self.send_frames(slot, now)
            ch = slot.messages
            if (ch.out and now - ch.last_send >= RESEND) or ch.ack_sent != ch.in_next:
                self.send_channel(slot, now)
        keep = min(s.frames_acked for s in self.slots if not s.dead)
        while self.frames_base < keep and self.frames:
            self.frames.popleft()
            self.frames_base += 1


# -- tunnel -----------------------------------------------------------------

class Tunnel(asyncio.DatagramProtocol):
    def __init__(self, lobby):
        self.lobby = lobby
        lobby.tunnel = self
        self.transport = None
        self.by_vip = {}
        self.by_addr = {}
        self.next_vip = 1
        self.matches = {}  # game id -> Match
        self.vip_match = {}  # vip -> (Match, slot index), while the match runs
        now = time.monotonic()
        self.register_rate = Bucket(REGISTER_RATE, REGISTER_BURST, now)
        self.register_ip_rate = {}
        self.register_log_time = now
        self.register_logged = 0
        self.register_unlogged = 0
        self.forwarded = 0
        self.dead_slots = 0
        self.desyncs = 0
        self.rejected = 0

    def connection_made(self, transport):
        self.transport = transport

    def error_received(self, exc):
        log.debug("udp error: %s", exc)

    # -- registration -----------------------------------------------------

    def allocate_vip(self, wanted):
        if wanted and (wanted & 0xFFFF0000) == VIP_NET and wanted not in self.by_vip:
            low = wanted & 0xFF
            if 0 < low < 255:
                return wanted
        for _ in range(65536):
            n = self.next_vip
            self.next_vip = self.next_vip % 65534 + 1
            vip = VIP_NET | n
            if 0 < (n & 0xFF) < 255 and vip not in self.by_vip:
                return vip
        return 0

    def admit(self, addr, now):
        """Rate limit for new registrations and rebinds."""
        bucket = self.register_ip_rate.get(addr[0])
        if not bucket:
            bucket = self.register_ip_rate[addr[0]] = Bucket(
                REGISTER_IP_RATE, REGISTER_IP_BURST, now)
        if bucket.allow(now) and self.register_rate.allow(now):
            return True
        self.rejected += 1
        return False

    def log_register(self, now, fmt, *args):
        if now - self.register_log_time >= 60:
            if self.register_unlogged:
                log.info("%d more registrations not logged", self.register_unlogged)
            self.register_log_time, self.register_logged, self.register_unlogged = now, 0, 0
        if self.register_logged < REGISTER_LOGS:
            self.register_logged += 1
            log.info(fmt, *args)
        else:
            self.register_unlogged += 1

    def forget(self, peer):
        """Drops a peer and what the lobby knows about its address."""
        if self.by_vip.get(peer.vip) is peer:
            del self.by_vip[peer.vip]
            for table in (self.lobby.rosters, self.lobby.versions, self.lobby.rtts):
                table.pop(peer.vip, None)
        if self.by_addr.get(peer.addr) is peer:
            del self.by_addr[peer.addr]

    def bind(self, vip, addr, token, now):
        old = self.by_vip.get(vip)
        if old:
            self.by_addr.pop(old.addr, None)
        stale = self.by_addr.get(addr)
        if stale:
            self.forget(stale)
        peer = Peer(vip, addr, token, now)
        if old:
            peer.version, peer.rtt_ms = old.version, old.rtt_ms
        self.by_vip[vip] = peer
        self.by_addr[addr] = peer
        return peer

    def movable(self, owner, token, now):
        """Whether a request presenting `token` may take `owner`'s address."""
        return now - owner.last_seen >= REBIND_AFTER and owner.token in (0, token)

    def register(self, addr, nonce, wanted, version, token, now):
        peer = self.by_addr.get(addr)
        if not peer:
            if not self.admit(addr, now):
                return None
            owner = self.by_vip.get(wanted)
            if owner and self.movable(owner, token, now):
                vip = wanted
            else:
                vip = self.allocate_vip(wanted)
                owner = None
            if not vip:
                log.warning("virtual address pool exhausted")
                return None
            new_token = (secrets.randbits(32) or 1) if version else 0
            peer = self.bind(vip, addr, owner.token if owner and owner.token else new_token,
                             now)
            self.log_register(now, "%s:%d registered as %s (v%d)", addr[0], addr[1],
                              format_ip(vip), version or 1)
        self.transport.sendto(HEADER.pack(TUNNEL_MAGIC, K_WELCOME) + struct.pack(
            "<IIII", nonce, peer.vip, peer.token, PROTOCOL_VERSION), addr)
        return peer

    def keepalive(self, addr, vip, token, now):
        peer = self.by_addr.get(addr)
        if peer or (vip & 0xFFFF0000) != VIP_NET or not 0 < (vip & 0xFF) < 255:
            return peer
        owner = self.by_vip.get(vip)
        if owner and not self.movable(owner, token, now):
            return None
        if not self.admit(addr, now):
            return None
        # After a server restart the address is free: it keeps the client's
        # token, so it stays protected.
        peer = self.bind(vip, addr, owner.token if owner else token, now)
        self.log_register(now, "%s:%d re-registered as %s", addr[0], addr[1], format_ip(vip))
        return peer

    def note(self, peer, roster, version, rtt_ms=None):
        lobby = self.lobby
        if lobby.rosters.get(peer.vip, 0) != roster:
            log.info("%s roster pack %s", format_ip(peer.vip),
                     f"{roster:08x}" if roster else "stock")
        if lobby.versions.get(peer.vip, 0) != version:
            log.info("%s protocol version %d", format_ip(peer.vip), version or 1)
        lobby.rosters[peer.vip] = roster
        lobby.versions[peer.vip] = peer.version = version
        if rtt_ms is not None:
            lobby.rtts[peer.vip] = peer.rtt_ms = rtt_ms

    # -- packets ----------------------------------------------------------

    def datagram_received(self, data, addr):
        if len(data) < HEADER.size or len(data) > MAX_DATAGRAM:
            return
        magic, kind = HEADER.unpack_from(data)
        if magic != TUNNEL_MAGIC:
            return
        now = time.monotonic()
        peer = self.by_addr.get(addr)
        if peer:
            if not peer.rate.allow(now):
                return
            peer.last_seen = now
        body = memoryview(data)[HEADER.size:]
        if kind == K_HELLO and len(body) >= 8:
            nonce, wanted, roster, version, token = u32s(body, 5)
            peer = self.register(addr, nonce, wanted, version, token, now)
            if peer:
                self.note(peer, roster, version)
        elif kind == K_KEEPALIVE and len(body) >= 4:
            vip, roster, version, token, seq, rtt_ms = u32s(body, 6)
            peer = self.keepalive(addr, vip, token, now)
            if peer:
                self.note(peer, roster, version, rtt_ms)
                if len(body) >= 20:
                    self.transport.sendto(HEADER.pack(TUNNEL_MAGIC, K_KEEPALIVE_ACK)
                                          + struct.pack("<II", seq, peer.vip), addr)
        elif not peer:
            return
        elif kind == K_DATA and len(body) >= 8:
            dest_vip, dest_port, src_port = struct.unpack_from("<IHH", body)
            dest = self.by_vip.get(dest_vip)
            if not dest:
                return
            self.transport.sendto(
                HEADER.pack(TUNNEL_MAGIC, K_DATA)
                + struct.pack("<IHH", peer.vip, src_port, dest_port) + body[8:], dest.addr)
            self.forwarded += 1
        elif kind == K_RELAY:
            self.relay_received(peer.vip, body)

    def send_relay(self, vip, packet):
        peer = self.by_vip.get(vip)
        if peer:
            self.transport.sendto(HEADER.pack(TUNNEL_MAGIC, K_RELAY) + packet, peer.addr)

    def relay_received(self, vip, data):
        if len(data) < RELAY_HEADER.size:
            return
        magic, kind, slot, count, _ = RELAY_HEADER.unpack_from(data)
        if magic != RELAY_MAGIC or not 3 <= count <= MAX_SLOTS or slot >= count:
            return
        at = RELAY_HEADER.size
        items = []
        if kind in (R_INPUTS, R_MESSAGES):
            if len(data) >= at + 10:
                n = struct.unpack_from("<H", data, at + 8)[0]
                items = read_items(data, at + 10, n,
                                   MAX_INPUT if kind == R_INPUTS else MAX_MESSAGE)
                if items is None:
                    return
        elif kind != R_HASH:
            return
        # A match in progress no longer needs the lobby (its players' lobby
        # connections may drop); only a new one is found through it.
        entry = self.vip_match.get(vip)
        if entry and not entry[0].slots[entry[1]].dead:
            match, bound = entry
            if count != match.count or slot != bound:
                return
        else:
            if kind == R_HASH:
                return
            game = self.lobby.game_of_ip(format_ip(vip))
            if not game:
                return
            match = self.matches.get(game.id)
            if match and match.count != count:
                return
            if not match:
                if count > len(game.players) or self.lobby.draining:
                    return
                match = self.matches[game.id] = Match(self, f"{game.id}/{count}", count)
                log.info("match %s: relay started", match.key)
        match.handle(kind, slot, vip, data, at, items)

    # -- housekeeping -----------------------------------------------------

    def end_match(self, key, match, why, now):
        match.log_slots(why, now)
        del self.matches[key]
        for slot in match.slots:
            if self.vip_match.get(slot.vip, (None,))[0] is match:
                del self.vip_match[slot.vip]

    def tick(self, now):
        for key, match in list(self.matches.items()):
            if now - match.last_packet > MATCH_IDLE:
                self.end_match(key, match, "relay ended", now)
            elif match.check_dead(now):
                self.end_match(key, match, "relay ended (every slot quiet or dropped)", now)
            else:
                if (not match.stall_logged and match.frames_end == 0
                        and now - match.started > STARTUP_STALL):
                    match.stall_logged = True
                    match.log_slots("no first frame", now)
                match.pump(now)

    def sweep(self, now):
        for peer in list(self.by_vip.values()):
            if now - peer.last_seen > PEER_TIMEOUT:
                log.info("%s timed out", format_ip(peer.vip))
                self.forget(peer)
                self.lobby.tunnel_lost(peer.vip)
        for ip, bucket in list(self.register_ip_rate.items()):
            if now - bucket.time > 60:
                del self.register_ip_rate[ip]

    async def run(self):
        last_sweep = time.monotonic()
        while True:
            await asyncio.sleep(0.005)
            now = time.monotonic()
            self.tick(now)
            if now - last_sweep >= 10:
                last_sweep = now
                self.sweep(now)

    def status(self):
        return {"tunnel_peers": len(self.by_vip), "relay_matches": len(self.matches),
                "dead_slots": self.dead_slots, "desyncs": self.desyncs,
                "registrations_rejected": self.rejected,
                "datagrams_forwarded": self.forwarded}
