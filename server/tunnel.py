"""UDP tunnel and lockstep relay host.

Every game registers with the tunnel and gets a virtual IPv4 address
(10.66.x.y) that it advertises as its own. Peer-to-peer datagrams to another
virtual address go to the server, which forwards them to that player, so
players never need open ports: each one only talks to the server.

Team play's lockstep relay (see src/nhl3on3_lockstep_relay.cpp) also runs
here: every console is a relay client and the server is the host, building
frame N once it holds input N from every slot.

Tunnel packets (little-endian), header: u32 magic, u8 kind, 3 reserved bytes
  kHello      c->s  u32 nonce, u32 wanted virtual address (0 = any)
  kWelcome    s->c  u32 nonce, u32 virtual address
  kKeepalive  c->s  u32 virtual address; also re-registers it after a server
                    restart or a NAT mapping change
  kData       c->s  u32 dest address, u16 dest port, u16 source port, payload
              s->c  u32 source address, u16 source port, u16 dest port, payload
  kRelay      both  one lockstep relay packet
"""

import asyncio
import logging
import struct
import time
from collections import deque

log = logging.getLogger("tunnel")

TUNNEL_MAGIC = 0x3154484E  # "NHT1"
K_HELLO, K_WELCOME, K_KEEPALIVE, K_DATA, K_RELAY = 1, 2, 3, 4, 5
HEADER = struct.Struct("<IB3x")
VIP_NET = (10 << 24) | (66 << 16)
PEER_TIMEOUT = 120.0
# A virtual address whose endpoint has been silent this long may move to a new
# endpoint (the same player after a NAT mapping change).
REBIND_AFTER = 15.0
MAX_DATAGRAM = 1500
RATE_PACKETS = 600  # per second per endpoint
RATE_BURST = 1200

RELAY_MAGIC = 0x3152484E  # "NHR1"
R_INPUTS, R_FRAMES, R_MESSAGES = 1, 2, 3
RELAY_HEADER = struct.Struct("<IBBBB")
MAX_RELAY_PACKET = 1400
MAX_SLOTS = 12
RESEND = 0.020
MATCH_IDLE = 30.0
STARTUP_STALL = 10.0  # seconds without a first frame before the slots are logged


def format_ip(ip):
    return f"{ip >> 24}.{(ip >> 16) & 255}.{(ip >> 8) & 255}.{ip & 255}"


def parse_ip(text):
    parts = [int(p) for p in text.split(".")]
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]


class Peer:
    def __init__(self, vip, addr):
        self.vip = vip
        self.addr = addr
        self.last_seen = time.monotonic()
        self.tokens = RATE_BURST
        self.token_time = self.last_seen

    def allow(self, now):
        self.tokens = min(RATE_BURST, self.tokens + (now - self.token_time) * RATE_PACKETS)
        self.token_time = now
        if self.tokens < 1:
            return False
        self.tokens -= 1
        return True


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

    @property
    def frames_end(self):
        return self.frames_base + len(self.frames)

    def log_slots(self, why, now):
        """Logs each slot's traffic, to show which console went quiet."""
        log.info("match %s: %s after %.1fs, %d frames built", self.key, why,
                 now - self.started, self.frames_end)
        for index, slot in enumerate(self.slots):
            ch = slot.messages
            quiet = f"{now - slot.last_packet:.1f}s ago" if slot.last_packet else "never"
            log.info("match %s:   slot %d %s: last packet %s, messages from it %d, "
                     "to it %d (%d unacknowledged), inputs %d, frames acked %d",
                     self.key, index, format_ip(slot.vip) if slot.vip else "-", quiet,
                     ch.in_next, ch.out_base + len(ch.out), len(ch.out),
                     slot.inputs_received, slot.frames_acked)

    def header(self, kind):
        return bytearray(RELAY_HEADER.pack(RELAY_MAGIC, kind, 0, self.count, 0))

    def send(self, slot, packet):
        self.tunnel.send_relay(slot.vip, bytes(packet))

    def handle(self, kind, slot_index, vip, data, at):
        slot = self.slots[slot_index]
        if slot.vip != vip:
            log.info("match %s: slot %d is %s", self.key, slot_index, format_ip(vip))
            slot.vip = vip
        self.last_packet = slot.last_packet = time.monotonic()
        if len(data) < at + 10:
            return
        acked, first, n = struct.unpack_from("<IIH", data, at)
        at += 10
        if kind == R_INPUTS:
            slot.frames_acked = max(slot.frames_acked, min(acked, self.frames_end))
            for i in range(n):
                got = read_input(data, at)
                if not got:
                    return
                item, at = got
                if first + i == slot.inputs_received:
                    slot.pending.append(item)
                    slot.inputs_received += 1
        elif kind == R_MESSAGES:
            ch = slot.messages
            while ch.out_base < acked and ch.out:
                ch.out.popleft()
                ch.out_base += 1
            for i in range(n):
                got = read_input(data, at)
                if not got:
                    return
                item, at = got
                if first + i != ch.in_next:
                    continue
                ch.in_next += 1
                for other_index, other in enumerate(self.slots):
                    if other_index != slot_index:
                        other.messages.out.append(item)
                        other.messages.last_send = 0.0
        self.pump(time.monotonic())

    def build_frames(self):
        while all(s.pending for s in self.slots):
            self.frames.append([s.pending.popleft() for s in self.slots])
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
        self.build_frames()
        end = self.frames_end
        for slot in self.slots:
            if not slot.vip:
                continue
            fresh = slot.frames_sent < end
            stale = slot.frames_acked < end and now - slot.last_send >= RESEND
            if fresh or stale or now - slot.last_send >= RESEND * 10:
                self.send_frames(slot, now)
            ch = slot.messages
            if (ch.out and now - ch.last_send >= RESEND) or ch.ack_sent != ch.in_next:
                self.send_channel(slot, now)
        keep = min(s.frames_acked for s in self.slots)
        while self.frames_base < keep and self.frames:
            self.frames.popleft()
            self.frames_base += 1


# -- tunnel -----------------------------------------------------------------

class Tunnel(asyncio.DatagramProtocol):
    def __init__(self, lobby):
        self.lobby = lobby
        self.transport = None
        self.by_vip = {}
        self.by_addr = {}
        self.next_vip = 1
        self.matches = {}
        self.forwarded = 0

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

    def bind(self, vip, addr):
        old = self.by_vip.get(vip)
        if old:
            self.by_addr.pop(old.addr, None)
        stale = self.by_addr.get(addr)
        if stale:
            self.by_vip.pop(stale.vip, None)
        peer = Peer(vip, addr)
        self.by_vip[vip] = peer
        self.by_addr[addr] = peer
        return peer

    def register(self, addr, nonce, wanted):
        peer = self.by_addr.get(addr)
        if not peer:
            owner = self.by_vip.get(wanted)
            if owner and time.monotonic() - owner.last_seen >= REBIND_AFTER:
                vip = wanted
            else:
                vip = self.allocate_vip(wanted)
            if not vip:
                log.warning("virtual address pool exhausted")
                return
            peer = self.bind(vip, addr)
            log.info("%s:%d registered as %s", addr[0], addr[1], format_ip(vip))
        self.transport.sendto(HEADER.pack(TUNNEL_MAGIC, K_WELCOME)
                              + struct.pack("<II", nonce, peer.vip), addr)

    def keepalive(self, addr, vip):
        if addr in self.by_addr or (vip & 0xFFFF0000) != VIP_NET:
            return
        owner = self.by_vip.get(vip)
        if owner and time.monotonic() - owner.last_seen < REBIND_AFTER:
            return
        self.bind(vip, addr)
        log.info("%s:%d re-registered as %s", addr[0], addr[1], format_ip(vip))

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
            if not peer.allow(now):
                return
            peer.last_seen = now
        body = memoryview(data)[HEADER.size:]
        if kind == K_HELLO and len(body) >= 8:
            nonce, wanted = struct.unpack_from("<II", body)
            self.register(addr, nonce, wanted)
        elif kind == K_KEEPALIVE and len(body) >= 4:
            self.keepalive(addr, struct.unpack_from("<I", body)[0])
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
        if kind not in (R_INPUTS, R_MESSAGES):
            return
        game = self.lobby.game_of_ip(format_ip(vip))
        if not game:
            return
        key = (game.id, count)
        match = self.matches.get(key)
        if not match:
            if count > len(game.players):
                return
            match = self.matches[key] = Match(self, f"{game.id}/{count}", count)
            log.info("match %s: relay started", match.key)
        match.handle(kind, slot, vip, data, RELAY_HEADER.size)

    # -- housekeeping -----------------------------------------------------

    async def run(self):
        last_sweep = time.monotonic()
        while True:
            await asyncio.sleep(0.005)
            now = time.monotonic()
            for key, match in list(self.matches.items()):
                if now - match.last_packet > MATCH_IDLE:
                    match.log_slots("relay ended", now)
                    del self.matches[key]
                else:
                    if (not match.stall_logged and match.frames_end == 0
                            and now - match.started > STARTUP_STALL):
                        match.stall_logged = True
                        match.log_slots("no first frame", now)
                    match.pump(now)
            if now - last_sweep >= 10:
                last_sweep = now
                for peer in list(self.by_vip.values()):
                    if now - peer.last_seen > PEER_TIMEOUT:
                        log.info("%s timed out", format_ip(peer.vip))
                        self.by_vip.pop(peer.vip, None)
                        self.by_addr.pop(peer.addr, None)

    def status(self):
        return {"tunnel_peers": len(self.by_vip), "relay_matches": len(self.matches),
                "datagrams_forwarded": self.forwarded}
