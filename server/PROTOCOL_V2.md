# Online protocol v2 changes

Every addition is an optional trailing field, so v1 clients and servers keep
working against v2 peers. All values are little-endian.

## Tunnel (UDP 3670, header `u32 magic "NHT1", u8 kind, 3 reserved`)

| Kind | Dir | Body (v1 fields, then **v2 additions**) |
| --- | --- | --- |
| 1 kHello | c->s | u32 nonce, u32 wanted vip, u32 roster, **u32 version, u32 token** |
| 2 kWelcome | s->c | u32 nonce, u32 vip, **u32 token, u32 server version** |
| 3 kKeepalive | c->s | u32 vip, u32 roster, **u32 version, u32 token, u32 seq, u32 rtt_ms** |
| 6 kKeepaliveAck (new) | s->c | u32 seq, u32 vip |

- `version` is `kNetProtocolVersion` = 2 (bump whenever gameplay
  determinism or the wire format changes). 0 or missing = v1.
- `token`: random u32 the server picks when it first binds a vip and returns
  in kWelcome. The client sends it back in later Hellos and keepalives (0 if
  it has none). A quiet vip (REBIND_AFTER) may only move to a new endpoint if
  the new request carries the same token, or the vip has no token (it was
  registered by a v1 client).
- The server answers every kKeepalive from a bound endpoint with kKeepaliveAck
  echoing `seq`. Clients measure RTT from it, report the smoothed value as
  `rtt_ms` in the next keepalive (0 = unknown), and treat 3 missed acks in a
  row (after at least one ack) as a lost server: they send kHello again with
  their current vip as `wanted` and their token.
- Quick match only pairs clients with the same `(roster, version)`.

## Lockstep relay (header `u32 magic "NHR1", u8 kind, u8 slot, u8 count, u8 reserved`)

- New kind 4 kHash (client -> host): u32 frame number, u32 state hash. Sent
  every 60 frames played. The host (the server in tunnel mode, slot 0
  otherwise) compares the hashes for the same frame number from every slot,
  logs the first mismatch per match with every slot's value, and ignores
  later ones. Hosts that don't know kind 4 drop it.
- Input data longer than 252 bytes (`kMaxInput`) and messages longer than
  1200 bytes are invalid. Receivers drop the whole packet.
- A frame can be bigger than one 1400-byte packet (12 slots x 255 bytes), so
  receive buffers must hold 64 KB datagrams.
- Disconnects: if a slot sends nothing for 8 s (or never sends in the first
  30 s of the match), the host marks it dead. From then on it fills that
  slot's input with type 1 (no input, length 0) and ignores anything the slot
  sends. Every console still plays the same frames, so they stay in sync.
- The host accepts no input more than 2 x kWindow (16) ahead of the frames it
  has built for that slot. It caps any slot's reliable-message queue at 256
  messages, and marks the slot dead if the cap is exceeded.
- Slot binding: a slot is bound to the first address or vip that sends for
  it. Packets for that slot from anywhere else are dropped (no rebinding).
