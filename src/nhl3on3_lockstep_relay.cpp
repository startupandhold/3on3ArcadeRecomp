// Lockstep input relay for 3+ console matches; see nhl3on3_lockstep_relay.h.
//
// Packets (UDP port kPort, little-endian), header:
//   u32 magic, u8 kind, u8 slot, u8 slot count, u8 reserved
// kInputs (client -> host): u32 frames received, u32 first input number,
//   u16 n, n x input
// kFrames (host -> client): u32 inputs received from that client, u32 first
//   frame number, u16 n, n x (slot count x input)
// kMessages (either way): u32 messages received, u32 first message number,
//   u16 n, n x input; control messages, reliable and in order per link
// kHash (client -> host): u32 frame number, u32 hash of the frames played up
//   to it, every kHashEvery frames; the host logs the first mismatch
// input: u8 type, u16 length, data (at most kMaxInput bytes, kMaxMessage for
//   messages; a packet with a longer one is dropped whole)
// Everything not yet acknowledged is resent until it is.
//
// The host (slot 0, or the server through the tunnel) binds each slot to the
// first address that sends for it, and marks a slot dead once it goes quiet
// (kDeadAfter, or kNeverHeardAfter if it never sent); a dead slot's input is
// "no input" (type 1) from then on, so every console keeps playing the same
// frames. See server/PROTOCOL_V2.md.

#include "nhl3on3_lockstep_relay.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

REXCVAR_DECLARE(int32_t, team_play);

// The match module's stream receive callback (stream, ?, ?, data, length):
// takes "gamedata" + u8 type + payload and posts it to the game.
extern "C" REX_FUNC(sub_82515070);

REXCVAR_DEFINE_INT32(lockstep_lead, 0, "NHL3on3/Online",
                     "Client inputs sent ahead of the frames played in a relayed match "
                     "(each is one input period of delay); 0 = from the measured turnaround");

REXCVAR_DEFINE_BOOL(relay_timing, false, "NHL3on3/Online",
                    "Log relayed match timing every 600 frames: how long the local "
                    "input takes to be built into a frame and then played");

REXCVAR_DEFINE_BOOL(lockstep_relay, true, "NHL3on3/Online",
                    "Carry a 3+ console match's lockstep input through the host "
                    "(needs --team_play)");

uint32_t Nhl3on3LocalIp();
std::vector<uint32_t> Nhl3on3GamePeers();
void Nhl3on3ClearGamePeers();
bool Nhl3on3TunnelActive();
bool Nhl3on3TunnelFellBack();
void Nhl3on3TunnelSendRelay(const uint8_t* data, size_t size);
bool Nhl3on3TunnelReceiveRelay(std::vector<uint8_t>& packet,
                               std::chrono::steady_clock::time_point& arrived);

namespace nhl3on3::lockstep_relay {
namespace {

using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
#endif

constexpr uint16_t kPort = 3660;
constexpr uint32_t kMagic = 0x3152484E;  // "NHR1"
constexpr uint8_t kInputs = 1;
constexpr uint8_t kFrames = 2;
constexpr uint8_t kMessages = 3;
constexpr uint8_t kHash = 4;
constexpr uint32_t kMaxMessage = 1200;
constexpr size_t kHeaderSize = 8;
constexpr size_t kMaxPacket = 1400;
// A frame can be bigger than one packet (12 slots x 255 bytes).
constexpr size_t kReceiveBuffer = 65536;
constexpr uint32_t kMaxInput = 252;  // the game's per-slot receive buffer
constexpr uint32_t kMaxSlots = 12;
constexpr uint32_t kWindow = 8;  // most local inputs sent ahead of the frames played
// Host: inputs taken per slot ahead of the frames built, and control
// messages queued for one client before it counts as gone.
constexpr uint32_t kMaxAhead = 2 * kWindow;
constexpr size_t kMaxQueued = 256;
// Host: a slot is dead once it sends nothing for kDeadAfter, or never sends
// in the first kNeverHeardAfter. Client: the host counts as lost after
// kHostLostAfter without a packet.
constexpr auto kDeadAfter = std::chrono::seconds(8);
constexpr auto kNeverHeardAfter = std::chrono::seconds(30);
constexpr auto kHostLostAfter = std::chrono::seconds(10);
constexpr uint32_t kHashEvery = 60;  // frames played between desync checks
constexpr size_t kHashCheckpoints = 16;  // host: checks held until every slot reports
constexpr auto kStallLogAfter = std::chrono::milliseconds(500);
constexpr auto kStallLogEvery = std::chrono::seconds(1);
constexpr uint32_t kMinLead = 1;
constexpr double kMinMarginMs = 4.0;
// After the first kSettleFrames, the lead drops only once it could have been
// lower for this long; each input it drops stalls every console one period.
constexpr uint32_t kSettleFrames = 120;
constexpr uint32_t kPlayedSlots = 64;  // > kWindow
constexpr auto kLowerAfter = std::chrono::seconds(3);
constexpr auto kResend = std::chrono::milliseconds(20);
constexpr auto kMaxResend = std::chrono::milliseconds(100);
constexpr uint32_t kDefaultRateMs = 16;  // if the dist has no input rate set
constexpr uint32_t kHashSeed = 2166136261u;  // FNV-1a

struct Input {
  uint8_t type = 1;
  std::vector<uint8_t> data;
};
using Frame = std::vector<Input>;  // one input per slot

// Reliable, ordered control messages over one link.
struct Channel {
  std::deque<Input> out;  // [out_base, out_base + out.size()) not yet acknowledged
  uint32_t out_base = 0;
  uint32_t in_next = 0;   // next message number expected
  uint32_t ack_sent = ~0u;
  uint32_t sent_end = 0;  // end of the messages sent at least once
  Clock::time_point last_send{};
};

struct Client {
  bool known = false;
  sockaddr_in addr{};  // the first address that sent for this slot
  // Gone (quiet too long or flooding): plays "no input" and is ignored.
  bool dead = false;
  Clock::time_point last_heard{};
  Channel messages;
  uint32_t inputs_received = 0;  // inputs taken from this client
  uint32_t frames_acked = 0;     // frames it has received
  uint32_t frames_sent = 0;      // frames sent so far (end of the last packet)
  Clock::time_point last_send{};
  std::deque<Input> pending;     // inputs not yet in a frame
  // Smoothed time from sending new frames to their ack (0 = unknown), from
  // one probe at a time: frames up to probe_frame, first sent at probe_sent.
  double rtt_ms = 0;
  uint32_t probe_frame = 0;
  Clock::time_point probe_sent{};
};

// Host: the hashes every slot reported for one frame number.
struct Checkpoint {
  uint32_t frame = 0;
  std::vector<std::optional<uint32_t>> hashes;  // by slot
};

struct Relay {
  uint32_t dist = 0, slot = 0, count = 0;
  bool host = false;
  // Packets go through the dedicated server's tunnel, and the server is the
  // host: every console, slot 0 included, is a client.
  bool tunnel = false;
  NativeSocket socket = kNoSocket;

  uint32_t inputs_sent = 0;   // local inputs produced
  uint32_t frames_played = 0; // frames handed to the game

  // Input pacing: the dist's input rate and when the next input is due.
  uint32_t rate_ms = 0;
  Clock::time_point next_input{};

  // Frames [frames_base, frames_base + frames.size()): built (host) or
  // received (client) and not yet played or acknowledged by every client.
  std::deque<Frame> frames;
  uint32_t frames_base = 0;

  // Client: local inputs [inputs_acked, inputs_sent) not yet taken by the host,
  // and when each could first be sent (frame N - lead played).
  std::deque<Input> unacked;
  std::deque<Clock::time_point> unacked_times;
  // Client: when each recent frame was played, by frame number mod kPlayedSlots.
  Clock::time_point played_at[kPlayedSlots]{};
  // Client: the input for frame N may be sent once frame N - lead is played,
  // and frame N must be back by the time it is due, lead input periods later.
  // turnaround_ms is the smoothed time from that send window opening to the
  // frame arriving (the trip through the server plus waiting for the other
  // consoles), turnaround_dev_ms its mean deviation.
  double turnaround_ms = 0;
  double turnaround_dev_ms = 0;
  uint32_t lead = kWindow;
  Clock::time_point lower_since{};
  uint32_t inputs_acked = 0;
  uint32_t inputs_put = 0;  // end of the local inputs sent at least once
  uint32_t acked_sent = ~0u;
  Clock::time_point last_send{};

  // Host: one entry per slot (its own slot's pending inputs included).
  std::vector<Client> clients;
  Clock::time_point started{};  // relay set up

  // Client: control messages to and from the host.
  Channel host_messages;
  // Client (direct): the host's address once a valid packet came from it;
  // until then, any game link peer's. Packets from anywhere else are dropped.
  bool host_bound = false;
  uint32_t host_ip = 0;
  Clock::time_point host_heard{};
  bool host_lost = false;
  bool logged_peers = false, logged_no_peers = false;
  uint32_t foreign_logged = 0;  // host: packets logged from a slot's other addresses

  // Received control messages not yet handed to the game.
  std::deque<Input> inbox;

  // Desync check: hash of every frame played so far; host: the checks still
  // waiting for some slot's hash.
  uint32_t hash = kHashSeed;
  std::deque<Checkpoint> checkpoints;
  bool desync_logged = false;

  // Diagnostics: when frame queries started coming back empty, when that was
  // last logged, and counters over the match.
  Clock::time_point stall_since{};
  Clock::time_point stall_logged{};
  uint64_t resends = 0;
  uint64_t dropped = 0;
  double stalled_ms = 0;  // in stalls longer than kStallLogAfter
};

std::mutex g_mutex;
Relay g_relay;

// relay_timing: per-frame times, indexed by frame number mod kTimingSlots,
// and sums over the current reporting period.
constexpr uint32_t kTimingSlots = 64;
constexpr uint32_t kTimingPeriod = 600;

struct TimingStats {
  double build_sum = 0, build_max = 0;  // input produced -> frame ready
  double wait_sum = 0, wait_max = 0;    // frame ready -> played
  double poll_sum = 0, poll_max = 0;    // between empty frame queries
  uint32_t frames = 0, polls = 0, backlog_sum = 0;
};

struct Timing {
  Clock::time_point sent[kTimingSlots]{};   // local input for frame N produced
  Clock::time_point built[kTimingSlots]{};  // frame N built (host) or received (client)
  Clock::time_point last_poll{};            // previous frame query that found none
  TimingStats stats;
};

Timing g_timing;

double Ms(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

void TimeFrameReady(uint32_t frame) {
  if (REXCVAR_GET(relay_timing)) {
    g_timing.built[frame % kTimingSlots] = Clock::now();
  }
}

// frame: 1-based number of the frame just played; backlog: frames still ready.
void TimeFramePlayed(uint32_t frame, uint32_t backlog) {
  if (!REXCVAR_GET(relay_timing)) {
    return;
  }
  Timing& t = g_timing;
  TimingStats& s = t.stats;
  const auto now = Clock::now();
  const auto sent = t.sent[frame % kTimingSlots], built = t.built[frame % kTimingSlots];
  t.sent[frame % kTimingSlots] = t.built[frame % kTimingSlots] = {};
  if (sent == Clock::time_point{} || built == Clock::time_point{}) {
    return;
  }
  const double build = Ms(built - sent), wait = Ms(now - built);
  s.build_sum += build;
  s.build_max = std::max(s.build_max, build);
  s.wait_sum += wait;
  s.wait_max = std::max(s.wait_max, wait);
  s.backlog_sum += backlog;
  if (++s.frames < kTimingPeriod) {
    return;
  }
  const Relay& r = g_relay;
  REXLOG_INFO(
      "[relay] timing to frame {}: input to frame ready avg {:.1f} max {:.1f} ms, ready to "
      "played avg {:.1f} max {:.1f} ms, total avg {:.1f} ms; frames left after each play "
      "avg {:.2f}; empty frame queries every {:.1f} ms (max {:.1f}); sent {} played {}; this "
      "match: resends {}, dropped packets {}, stalled {:.0f} ms",
      frame, s.build_sum / s.frames, s.build_max, s.wait_sum / s.frames, s.wait_max,
      (s.build_sum + s.wait_sum) / s.frames, double(s.backlog_sum) / s.frames,
      s.polls ? s.poll_sum / s.polls : 0.0, s.poll_max, r.inputs_sent, r.frames_played,
      r.resends, r.dropped, r.stalled_ms);
  s = TimingStats{};
}

void TimeEmptyQuery() {
  if (!REXCVAR_GET(relay_timing)) {
    return;
  }
  Timing& t = g_timing;
  const auto now = Clock::now();
  if (t.last_poll != Clock::time_point{}) {
    const double gap = Ms(now - t.last_poll);
    t.stats.poll_sum += gap;
    t.stats.poll_max = std::max(t.stats.poll_max, gap);
    ++t.stats.polls;
  }
  t.last_poll = now;
}

std::string FormatIp(uint32_t ip) {
  return fmt::format("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

uint32_t Load32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void Store32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

void CloseSocket(NativeSocket s) {
#if defined(_WIN32)
  closesocket(s);
#else
  close(s);
#endif
}

NativeSocket OpenSocket(uint32_t ip) {
#if defined(_WIN32)
  static const bool started = [] {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  (void)started;
#endif
  NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoSocket) {
    return s;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = htonl(ip);
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseSocket(s);
    return kNoSocket;
  }
#if defined(_WIN32)
  u_long nonblocking = 1;
  ioctlsocket(s, FIONBIO, &nonblocking);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
  return s;
}

class Writer {
 public:
  explicit Writer(std::vector<uint8_t>& out) : out_(out) {}
  void U8(uint8_t v) { out_.push_back(v); }
  void U16(uint16_t v) {
    out_.push_back(uint8_t(v));
    out_.push_back(uint8_t(v >> 8));
  }
  void U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      out_.push_back(uint8_t(v >> (i * 8)));
    }
  }
  void Put(const Input& in) {
    U8(in.type);
    U16(uint16_t(in.data.size()));
    out_.insert(out_.end(), in.data.begin(), in.data.end());
  }

 private:
  std::vector<uint8_t>& out_;
};

class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
  bool ok() const { return ok_; }
  uint8_t U8() { return Need(1) ? *p_++ : 0; }
  uint16_t U16() {
    if (!Need(2)) return 0;
    const uint16_t v = uint16_t(p_[0] | (p_[1] << 8));
    p_ += 2;
    return v;
  }
  uint32_t U32() {
    if (!Need(4)) return 0;
    const uint32_t v = uint32_t(p_[0]) | (uint32_t(p_[1]) << 8) | (uint32_t(p_[2]) << 16) |
                       (uint32_t(p_[3]) << 24);
    p_ += 4;
    return v;
  }
  // An input of at most `max` data bytes; a longer one fails the packet.
  Input Get(uint32_t max) {
    Input in;
    in.type = U8();
    const uint16_t length = U16();
    if (length > max) {
      ok_ = false;
    }
    if (Need(length)) {
      in.data.assign(p_, p_ + length);
      p_ += length;
    }
    return in;
  }

 private:
  bool Need(size_t n) {
    ok_ = ok_ && size_t(end_ - p_) >= n;
    return ok_;
  }
  const uint8_t* p_;
  const uint8_t* end_;
  bool ok_ = true;
};

size_t InputSize(const Input& in) {
  return 3 + in.data.size();
}

void Header(Writer& w, uint8_t kind) {
  w.U32(kMagic);
  w.U8(kind);
  w.U8(uint8_t(g_relay.slot));
  w.U8(uint8_t(g_relay.count));
  w.U8(0);
}

void SendTo(const sockaddr_in& addr, const std::vector<uint8_t>& packet) {
  if (g_relay.tunnel) {
    Nhl3on3TunnelSendRelay(packet.data(), packet.size());
    return;
  }
  ::sendto(g_relay.socket, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
           reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

uint32_t FramesEnd() {
  return g_relay.frames_base + uint32_t(g_relay.frames.size());
}

std::string FormatAddr(const sockaddr_in& addr) {
  return fmt::format("{}:{}", FormatIp(ntohl(addr.sin_addr.s_addr)), ntohs(addr.sin_port));
}

bool SameAddr(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// Host: how long to wait before resending to a client; half its round trip
// once that is known, within [kResend, kMaxResend].
Clock::duration ResendInterval(const Client& c) {
  if (c.rtt_ms <= 0) {
    return kResend;
  }
  const auto half = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(c.rtt_ms / 2));
  return std::clamp<Clock::duration>(half, kResend, kMaxResend);
}

// Host: sends a client every frame it has not acknowledged, as many as fit.
void SendFrames(Client& client, Clock::time_point now) {
  Relay& r = g_relay;
  const uint32_t first = std::max(client.frames_acked, r.frames_base);
  std::vector<uint8_t> packet;
  Writer w(packet);
  Header(w, kFrames);
  w.U32(client.inputs_received);
  w.U32(first);
  const size_t count_at = packet.size();
  w.U16(0);
  uint16_t n = 0;
  for (uint32_t f = first; f < FramesEnd(); ++f) {
    const Frame& frame = r.frames[f - r.frames_base];
    size_t size = 0;
    for (const Input& in : frame) {
      size += InputSize(in);
    }
    if (n && packet.size() + size > kMaxPacket) {
      break;
    }
    for (const Input& in : frame) {
      w.Put(in);
    }
    ++n;
  }
  packet[count_at] = uint8_t(n);
  packet[count_at + 1] = uint8_t(n >> 8);
  SendTo(client.addr, packet);
  if (n && first + n <= client.frames_sent) {  // nothing new
    ++r.resends;
  }
  // Time one batch of new frames to its ack at a time.
  if (n && !client.probe_frame && first + n > client.frames_sent) {
    client.probe_frame = first + n;
    client.probe_sent = now;
  }
  client.frames_sent = first + n;
  client.last_send = now;
}

// Host: the slot's client stops counting: its input is "no input" from the
// next frame built, and nothing it sends is taken.
void MarkDead(uint32_t slot, const char* why) {
  Relay& r = g_relay;
  Client& c = r.clients[slot];
  if (c.dead) {
    return;
  }
  c.dead = true;
  c.pending.clear();
  c.messages.out.clear();
  REXLOG_WARN("[relay] host: slot {} ({}) is gone: {}; it plays no input from frame {} on",
              slot, c.known ? FormatAddr(c.addr) : "never heard from", why, FramesEnd() + 1);
}

// Host: queues a control message for slot `slot`; a client that lets
// kMaxQueued pile up counts as gone.
void QueueMessage(uint32_t slot, const Input& in) {
  Client& c = g_relay.clients[slot];
  if (c.dead) {
    return;
  }
  if (c.messages.out.size() >= kMaxQueued) {
    MarkDead(slot, "too many unacknowledged messages");
    return;
  }
  c.messages.out.push_back(in);
}

// Host: marks slots that went quiet dead.
void CheckSlots(Clock::time_point now) {
  Relay& r = g_relay;
  for (uint32_t s = 0; s < r.count; ++s) {
    const Client& c = r.clients[s];
    if (s == r.slot || c.dead) {
      continue;
    }
    if (c.known && now - c.last_heard >= kDeadAfter) {
      MarkDead(s, "nothing received for 8 s");
    } else if (!c.known && now - r.started >= kNeverHeardAfter) {
      MarkDead(s, "nothing received in the first 30 s");
    }
  }
}

// Host: turns inputs held from every slot into frames.
void BuildFrames() {
  Relay& r = g_relay;
  for (;;) {
    for (const Client& c : r.clients) {
      if (!c.dead && c.pending.empty()) {
        return;
      }
    }
    Frame frame;
    frame.reserve(r.clients.size());
    for (Client& c : r.clients) {
      if (c.dead) {
        frame.emplace_back();  // type 1, no input
        continue;
      }
      frame.push_back(std::move(c.pending.front()));
      c.pending.pop_front();
    }
    r.frames.push_back(std::move(frame));
    const uint32_t built = FramesEnd();
    TimeFrameReady(built);
    if (built == 1 || built % 600 == 0) {
      REXLOG_INFO("[relay] host built frame {} (played {})", built, r.frames_played);
    }
  }
}

// Host: drops frames every live client has acknowledged and the host has
// played.
void TrimFrames() {
  Relay& r = g_relay;
  uint32_t keep = r.frames_played;
  for (uint32_t s = 0; s < r.count; ++s) {
    if (s != r.slot && !r.clients[s].dead) {
      keep = std::min(keep, r.clients[s].frames_acked);
    }
  }
  while (r.frames_base < keep && !r.frames.empty()) {
    r.frames.pop_front();
    ++r.frames_base;
  }
}

// Host: the client for a packet from `from` claiming `slot`, binding the
// slot to the first address that sends for it; null (and counted as
// dropped) for the host's own slot, a dead slot or another address.
Client* ClientFor(uint8_t slot, const sockaddr_in& from, Clock::time_point now) {
  Relay& r = g_relay;
  if (slot >= r.count || slot == r.slot) {
    ++r.dropped;
    return nullptr;
  }
  Client& c = r.clients[slot];
  if (c.dead) {
    ++r.dropped;
    return nullptr;
  }
  if (!c.known) {
    c.known = true;
    c.addr = from;
    REXLOG_INFO("[relay] host: slot {} is {}", slot, FormatAddr(from));
  } else if (!SameAddr(c.addr, from)) {
    ++r.dropped;
    if (r.foreign_logged < 8) {
      ++r.foreign_logged;
      REXLOG_WARN("[relay] host: dropped a packet for slot {} from {} (slot is {})", slot,
                  FormatAddr(from), FormatAddr(c.addr));
    }
    return nullptr;
  }
  c.last_heard = now;
  return &c;
}

void HandleInputs(Reader& rd, uint8_t slot, const sockaddr_in& from, Clock::time_point arrived) {
  Relay& r = g_relay;
  const uint32_t acked = rd.U32();
  const uint32_t first = rd.U32();
  const uint16_t n = rd.U16();
  std::vector<Input> inputs;
  inputs.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    inputs.push_back(rd.Get(kMaxInput));
  }
  if (!rd.ok()) {
    ++r.dropped;
    return;
  }
  Client* c = ClientFor(slot, from, arrived);
  if (!c) {
    return;
  }
  c->frames_acked = std::max(c->frames_acked, std::min(acked, FramesEnd()));
  if (c->probe_frame && c->frames_acked >= c->probe_frame) {
    const double sample = Ms(arrived - c->probe_sent);
    c->rtt_ms = c->rtt_ms > 0 ? c->rtt_ms + (sample - c->rtt_ms) * 0.125 : sample;
    c->probe_frame = 0;
  }
  for (uint32_t i = 0; i < n; ++i) {
    // Inputs too far ahead of the frames built are left for a resend.
    if (first + i == c->inputs_received && c->pending.size() < kMaxAhead) {
      c->pending.push_back(std::move(inputs[i]));
      ++c->inputs_received;
    }
  }
}

// Client: whether a packet from `from` comes from the host: through the
// tunnel always; directly, from the address frames first came from, or
// before that from a game link peer. Binds the host's address if `bind`.
bool FromHost(const sockaddr_in& from, bool bind) {
  Relay& r = g_relay;
  if (r.tunnel) {
    return true;
  }
  const uint32_t ip = ntohl(from.sin_addr.s_addr);
  if (r.host_bound) {
    return ip == r.host_ip;
  }
  const std::vector<uint32_t> peers = Nhl3on3GamePeers();
  if (std::find(peers.begin(), peers.end(), ip) == peers.end()) {
    return false;
  }
  if (bind) {
    r.host_bound = true;
    r.host_ip = ip;
    REXLOG_INFO("[relay] client: host is {}", FormatAddr(from));
  }
  return true;
}

// Client: a valid packet came from the host.
void HeardHost(Clock::time_point now) {
  Relay& r = g_relay;
  if (r.host_lost) {
    r.host_lost = false;
    REXLOG_INFO("[relay] client: the host is back after {:.1f} s", Ms(now - r.host_heard) / 1000);
  }
  r.host_heard = now;
}

// Adds a played frame to the running desync hash (FNV-1a over each slot's
// type, length and data as handed to the game). No guest addresses of the
// match state (score, clock, puck and player positions) are known, so this
// only catches consoles playing different frames (a relay fault), not a
// simulation that diverged on the same frames.
void HashFrame(const Frame& frame) {
  uint32_t h = g_relay.hash;
  const auto mix = [&h](uint8_t byte) { h = (h ^ byte) * 16777619u; };
  for (const Input& in : frame) {
    const uint32_t length =
        in.type == 1 ? 0 : std::min<uint32_t>(uint32_t(in.data.size()), kMaxInput);
    mix(in.type);
    mix(uint8_t(length));
    mix(uint8_t(length >> 8));
    for (uint32_t i = 0; i < length; ++i) {
      mix(in.data[i]);
    }
  }
  g_relay.hash = h;
}

// Host: logs a check's hashes if any two slots disagree; only the first
// mismatch of a match is logged.
void CompareCheckpoint(const Checkpoint& cp) {
  Relay& r = g_relay;
  std::optional<uint32_t> seen;
  bool mismatch = false;
  for (const auto& h : cp.hashes) {
    if (h) {
      mismatch = mismatch || (seen && *seen != *h);
      seen = h;
    }
  }
  if (!mismatch || r.desync_logged) {
    return;
  }
  r.desync_logged = true;
  std::string list;
  for (uint32_t s = 0; s < cp.hashes.size(); ++s) {
    list += fmt::format("{}{}:{}", list.empty() ? "" : " ", s,
                        cp.hashes[s] ? fmt::format("{:08X}", *cp.hashes[s]) : "-");
  }
  REXLOG_ERROR("[relay] DESYNC: slots played different frames by frame {} (hashes {}); the "
               "hash covers the relayed inputs, not the game state",
               cp.frame, list);
}

// Host: one slot's hash of the frames up to `frame`. A check is compared once
// every live slot has reported, or when it is pushed out by newer ones.
void RecordHash(uint32_t slot, uint32_t frame, uint32_t hash) {
  Relay& r = g_relay;
  if (r.desync_logged || slot >= r.count) {
    return;
  }
  auto it = std::find_if(r.checkpoints.begin(), r.checkpoints.end(),
                         [frame](const Checkpoint& cp) { return cp.frame == frame; });
  if (it == r.checkpoints.end()) {
    if (r.checkpoints.size() >= kHashCheckpoints) {
      CompareCheckpoint(r.checkpoints.front());
      r.checkpoints.pop_front();
    }
    Checkpoint cp;
    cp.frame = frame;
    cp.hashes.resize(r.count);
    r.checkpoints.push_back(std::move(cp));
    it = std::prev(r.checkpoints.end());
  }
  it->hashes[slot] = hash;
  for (uint32_t s = 0; s < r.count; ++s) {
    if (!it->hashes[s] && (s == r.slot || !r.clients[s].dead)) {
      return;
    }
  }
  CompareCheckpoint(*it);
  r.checkpoints.erase(it);
}

void HandleHash(Reader& rd, uint8_t slot, const sockaddr_in& from, Clock::time_point arrived) {
  const uint32_t frame = rd.U32();
  const uint32_t hash = rd.U32();
  if (!rd.ok()) {
    ++g_relay.dropped;
    return;
  }
  if (ClientFor(slot, from, arrived)) {
    RecordHash(slot, frame, hash);
  }
}

// Client: the lead from lockstep_lead, or enough input periods to cover the
// turnaround plus twice its deviation.
void UpdateLead(Clock::time_point now) {
  Relay& r = g_relay;
  uint32_t lead = uint32_t(std::max(REXCVAR_GET(lockstep_lead), 0));
  const bool fixed = lead != 0;
  if (!fixed && r.rate_ms && r.turnaround_ms > 0) {
    const double needed =
        r.turnaround_ms + std::max(2.0 * r.turnaround_dev_ms, kMinMarginMs);
    lead = uint32_t(std::ceil(needed / r.rate_ms));
  }
  lead = lead ? std::clamp(lead, kMinLead, kWindow) : kWindow;
  // Settle freely over the first frames (the match is still starting up).
  if (lead < r.lead && !fixed && r.frames_played >= kSettleFrames) {
    if (r.lower_since == Clock::time_point{}) {
      r.lower_since = now;
    }
    if (now - r.lower_since < kLowerAfter) {
      return;
    }
  }
  r.lower_since = {};
  if (lead != r.lead) {
    REXLOG_INFO("[relay] client lead {} inputs (turnaround {:.0f} +/- {:.0f} ms, input rate {} ms)",
                lead, r.turnaround_ms, r.turnaround_dev_ms, r.rate_ms);
    r.lead = lead;
  }
}

void HandleFrames(Reader& rd, uint8_t count, const sockaddr_in& from, Clock::time_point arrived) {
  Relay& r = g_relay;
  if (count != r.count) {
    return;
  }
  const uint32_t inputs_received = rd.U32();
  const uint32_t first = rd.U32();
  const uint16_t n = rd.U16();
  std::vector<Frame> frames(n, Frame(r.count));
  for (Frame& frame : frames) {
    for (Input& in : frame) {
      in = rd.Get(kMaxInput);
    }
  }
  if (!rd.ok() || !FromHost(from, true)) {
    ++r.dropped;
    return;
  }
  HeardHost(arrived);
  while (r.inputs_acked < inputs_received && !r.unacked.empty()) {
    const double sample = Ms(arrived - r.unacked_times.front());
    if (r.turnaround_ms > 0) {
      r.turnaround_dev_ms += (std::fabs(sample - r.turnaround_ms) - r.turnaround_dev_ms) * 0.25;
      r.turnaround_ms += (sample - r.turnaround_ms) * 0.125;
    } else {
      r.turnaround_ms = sample;
      r.turnaround_dev_ms = sample / 2;
    }
    r.unacked.pop_front();
    r.unacked_times.pop_front();
    ++r.inputs_acked;
  }
  UpdateLead(Clock::now());
  for (uint32_t i = 0; i < n; ++i) {
    if (first + i == FramesEnd()) {
      r.frames.push_back(std::move(frames[i]));
      const uint32_t received = FramesEnd();
      TimeFrameReady(received);
      if (received == 1 || received % 600 == 0) {
        REXLOG_INFO("[relay] client received frame {} (played {})", received, r.frames_played);
      }
    }
  }
}

// Client: where the host may be; every address its game link talks to, or
// only the host's once known.
std::vector<sockaddr_in> HostAddrs() {
  if (g_relay.tunnel) {
    return {sockaddr_in{}};  // the server; SendTo ignores the address
  }
  std::vector<uint32_t> ips;
  if (g_relay.host_bound) {
    ips.push_back(g_relay.host_ip);
  } else {
    ips = Nhl3on3GamePeers();
  }
  std::vector<sockaddr_in> out;
  for (uint32_t ip : ips) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(ip);
    out.push_back(addr);
  }
  return out;
}

// Sends a link's unacknowledged control messages and its receive ack.
void SendChannel(Channel& ch, const std::vector<sockaddr_in>& to, Clock::time_point now) {
  std::vector<uint8_t> packet;
  Writer w(packet);
  Header(w, kMessages);
  w.U32(ch.in_next);
  w.U32(ch.out_base);
  const size_t count_at = packet.size();
  w.U16(0);
  uint16_t n = 0;
  for (const Input& in : ch.out) {
    if (n && packet.size() + InputSize(in) > kMaxPacket) {
      break;
    }
    w.Put(in);
    ++n;
  }
  packet[count_at] = uint8_t(n);
  packet[count_at + 1] = uint8_t(n >> 8);
  for (const sockaddr_in& addr : to) {
    SendTo(addr, packet);
  }
  if (n && ch.out_base + n <= ch.sent_end) {  // nothing new
    ++g_relay.resends;
  }
  ch.sent_end = std::max(ch.sent_end, ch.out_base + n);
  ch.ack_sent = ch.in_next;
  ch.last_send = now;
}

void PumpChannel(Channel& ch, const std::vector<sockaddr_in>& to, Clock::time_point now,
                 Clock::duration interval = kResend) {
  const bool resend = !ch.out.empty() && now - ch.last_send >= interval;
  if (resend || ch.ack_sent != ch.in_next) {
    SendChannel(ch, to, now);
  }
}

void HandleMessages(Reader& rd, uint8_t slot, const sockaddr_in& from, Clock::time_point arrived) {
  Relay& r = g_relay;
  const uint32_t acked = rd.U32();
  const uint32_t first = rd.U32();
  const uint16_t n = rd.U16();
  std::vector<Input> messages;
  messages.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    messages.push_back(rd.Get(kMaxMessage));
  }
  if (!rd.ok()) {
    ++r.dropped;
    return;
  }
  Channel* ch = &r.host_messages;
  if (r.host) {
    Client* c = ClientFor(slot, from, arrived);
    if (!c) {
      return;
    }
    ch = &c->messages;
  } else if (!FromHost(from, true)) {
    ++r.dropped;
    return;
  } else {
    HeardHost(arrived);
  }
  while (ch->out_base < acked && !ch->out.empty()) {
    ch->out.pop_front();
    ++ch->out_base;
  }
  for (uint32_t i = 0; i < n; ++i) {
    if (first + i != ch->in_next) {
      continue;
    }
    ++ch->in_next;
    Input& in = messages[i];
    if (r.host) {
      for (uint32_t s = 0; s < r.count; ++s) {
        if (s != slot && s != r.slot) {
          QueueMessage(s, in);
        }
      }
    }
    REXLOG_INFO("[relay] message type {} len {} from slot {}", in.type, in.data.size(), slot);
    r.inbox.push_back(std::move(in));
  }
}

// Client: sends every input the host has not taken, and the frame ack.
void SendInputs(Clock::time_point now) {
  Relay& r = g_relay;
  std::vector<uint8_t> packet;
  Writer w(packet);
  Header(w, kInputs);
  w.U32(FramesEnd());
  w.U32(r.inputs_acked);
  const size_t count_at = packet.size();
  w.U16(0);
  uint16_t n = 0;
  for (const Input& in : r.unacked) {
    if (n && packet.size() + InputSize(in) > kMaxPacket) {
      break;
    }
    w.Put(in);
    ++n;
  }
  packet[count_at] = uint8_t(n);
  packet[count_at + 1] = uint8_t(n >> 8);
  for (const sockaddr_in& addr : HostAddrs()) {
    SendTo(addr, packet);
  }
  if (n && r.inputs_acked + n <= r.inputs_put) {  // nothing new
    ++r.resends;
  }
  r.inputs_put = std::max(r.inputs_put, r.inputs_acked + n);
  r.acked_sent = FramesEnd();
  r.last_send = now;
  if (r.tunnel) {
    return;
  }
  const std::vector<uint32_t> peers = Nhl3on3GamePeers();
  if (peers.empty() && !r.logged_no_peers) {
    r.logged_no_peers = true;
    REXLOG_WARN("[relay] client: no game link peers known yet; cannot reach the host");
  }
  if (!r.logged_peers && !peers.empty()) {
    r.logged_peers = true;
    std::string list;
    for (uint32_t ip : peers) {
      list += (list.empty() ? "" : ", ") + FormatIp(ip);
    }
    REXLOG_INFO("[relay] client: sending inputs to {}", list);
  }
}

// Client: tells the host the hash of the frames played so far.
void SendHash(uint32_t frame, uint32_t hash) {
  std::vector<uint8_t> packet;
  Writer w(packet);
  Header(w, kHash);
  w.U32(frame);
  w.U32(hash);
  for (const sockaddr_in& addr : HostAddrs()) {
    SendTo(addr, packet);
  }
}

// Reads the next packet into `buffer`; its length, or -1 if none is waiting.
int ReceiveOne(uint8_t* buffer, size_t capacity, sockaddr_in& from, Clock::time_point& arrived) {
  Relay& r = g_relay;
  arrived = Clock::now();
  if (r.tunnel) {
    std::vector<uint8_t> packet;
    for (;;) {
      if (!Nhl3on3TunnelReceiveRelay(packet, arrived)) {
        return -1;
      }
      if (packet.size() <= capacity) {
        break;
      }
      ++r.dropped;
    }
    std::memcpy(buffer, packet.data(), packet.size());
    return int(packet.size());
  }
  for (;;) {
#if defined(_WIN32)
    int from_len = sizeof(from);
#else
    socklen_t from_len = sizeof(from);
#endif
    const int n = ::recvfrom(r.socket, reinterpret_cast<char*>(buffer), int(capacity), 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
    arrived = Clock::now();
#if defined(_WIN32)
    if (n < 0 && (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE)) {
      continue;
    }
#endif
    return n;
  }
}

void Receive() {
  Relay& r = g_relay;
  // Only used under g_mutex; too big for the guest thread's stack.
  static std::vector<uint8_t> buffer(kReceiveBuffer);
  for (;;) {
    sockaddr_in from{};
    Clock::time_point arrived;
    const int n = ReceiveOne(buffer.data(), buffer.size(), from, arrived);
    if (n < 0) {
      return;
    }
    if (size_t(n) < kHeaderSize) {
      continue;
    }
    Reader rd(buffer.data(), size_t(n));
    if (rd.U32() != kMagic) {
      continue;
    }
    const uint8_t kind = rd.U8(), slot = rd.U8(), count = rd.U8();
    rd.U8();
    if (count != r.count) {
      continue;
    }
    if (r.host && kind == kInputs) {
      HandleInputs(rd, slot, from, arrived);
    } else if (!r.host && kind == kFrames) {
      HandleFrames(rd, count, from, arrived);
    } else if (kind == kMessages && (r.host || slot == 0)) {
      HandleMessages(rd, slot, from, arrived);
    } else if (r.host && kind == kHash) {
      HandleHash(rd, slot, from, arrived);
    }
  }
}

// Exchanges packets; called from every hooked dist call.
void Pump() {
  Relay& r = g_relay;
  if (r.socket == kNoSocket && !r.tunnel) {
    return;
  }
  Receive();
  const auto now = Clock::now();
  if (r.host) {
    CheckSlots(now);
    BuildFrames();
    for (uint32_t s = 0; s < r.count; ++s) {
      Client& c = r.clients[s];
      if (s == r.slot || !c.known || c.dead) {
        continue;
      }
      const auto resend = ResendInterval(c);
      const bool fresh = c.frames_sent < FramesEnd();
      const bool stale = c.frames_acked < FramesEnd() && now - c.last_send >= resend;
      if (fresh || stale || now - c.last_send >= kResend * 10) {
        SendFrames(c, now);
      }
      PumpChannel(c.messages, {c.addr}, now, resend);
    }
    TrimFrames();
  } else {
    const bool fresh = !r.unacked.empty() && now - r.last_send >= kResend;
    if (fresh || r.acked_sent != FramesEnd() || now - r.last_send >= kResend * 10) {
      SendInputs(now);
    }
    PumpChannel(r.host_messages, HostAddrs(), now);
    if (!r.host_lost && now - r.host_heard >= kHostLostAfter) {
      r.host_lost = true;
      REXLOG_ERROR("[relay] client: nothing from the {} for {:.0f} s (frames played {}); the "
                   "match cannot go on until it answers. If the host has left, the game's own "
                   "link timeout should end the match",
                   r.tunnel ? "server" : "host", Ms(now - r.host_heard) / 1000, r.frames_played);
    }
  }
}
// A frame query found no frame: once that has lasted kStallLogAfter, logs
// what the relay waits on, once per kStallLogEvery.
void NoteStall(Clock::time_point now) {
  Relay& r = g_relay;
  if (r.stall_since == Clock::time_point{}) {
    r.stall_since = now;
    return;
  }
  if (now - r.stall_since < kStallLogAfter || now - r.stall_logged < kStallLogEvery) {
    return;
  }
  r.stall_logged = now;
  const double stalled = Ms(now - r.stall_since);
  if (!r.host) {
    REXLOG_WARN("[relay] waiting on {} frames for {:.0f} ms (frame {}, last packet {:.0f} ms "
                "ago)",
                r.tunnel ? "the server's" : "host", stalled, r.frames_played + 1,
                Ms(now - r.host_heard));
    return;
  }
  for (uint32_t s = 0; s < r.count; ++s) {
    const Client& c = r.clients[s];
    if (c.dead || !c.pending.empty()) {
      continue;
    }
    if (s == r.slot) {
      REXLOG_WARN("[relay] waiting on slot {} (local input) for {:.0f} ms", s, stalled);
    } else {
      REXLOG_WARN("[relay] waiting on slot {} for {:.0f} ms (last packet {})", s, stalled,
                  c.known ? fmt::format("{:.0f} ms ago", Ms(now - c.last_heard)) : "never");
    }
  }
}

// A frame was played: ends a stall.
void EndStall(Clock::time_point now) {
  Relay& r = g_relay;
  if (r.stall_since != Clock::time_point{} && now - r.stall_since >= kStallLogAfter) {
    if (r.frames_played > 0) {  // not waiting for the match to start
      r.stalled_ms += Ms(now - r.stall_since);
    }
    REXLOG_INFO("[relay] frame {} arrived after a {:.0f} ms stall", r.frames_played + 1,
                Ms(now - r.stall_since));
  }
  r.stall_since = {};
  r.stall_logged = {};
}

bool Enabled() {
  return REXCVAR_GET(lockstep_relay) && REXCVAR_GET(team_play) >= 3;
}

}  // namespace

void OnMultiSetup(uint32_t dist, uint32_t slot, uint32_t count) {
  if (!Enabled() || count < 3 || count > kMaxSlots || slot >= count) {
    return;
  }
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  if (r.dist == dist && r.slot == slot && r.count == count) {
    return;
  }
  if (r.socket != kNoSocket) {
    CloseSocket(r.socket);
  }
  r = Relay{};
  g_timing = Timing{};
  r.dist = dist;
  r.slot = slot;
  r.count = count;
  r.clients.resize(count);
  r.started = r.host_heard = Clock::now();
  if (Nhl3on3TunnelActive()) {
    r.tunnel = true;
    REXLOG_INFO("[relay] dist {:08X}: slot {} of {}, hosted by the server", dist, slot, count);
    return;
  }
  r.host = slot == 0;
  const uint32_t ip = Nhl3on3LocalIp();
  r.socket = OpenSocket(ip);
  if (r.socket == kNoSocket) {
    REXLOG_ERROR("[relay] could not bind {}:{}; lockstep stays on the game's links",
                 FormatIp(ip), kPort);
    r.dist = 0;
    return;
  }
  REXLOG_INFO("[relay] dist {:08X}: slot {} of {} ({}), listening on {}:{}", dist, slot, count,
              r.host ? "host" : "client", FormatIp(ip), kPort);
  if (Nhl3on3TunnelFellBack()) {
    REXLOG_WARN("[relay] the server tunnel did not answer at startup, so this match is played "
                "DIRECT: every console must reach the host on UDP port {} itself",
                kPort);
  }
}

void OnDistDestroyed(uint32_t dist) {
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  if (!dist || dist != r.dist) {
    return;
  }
  REXLOG_INFO("[relay] dist {:08X} destroyed: slot {} of {}, inputs sent {} (taken {}), frames "
              "played {}, messages received {}, sent {} ({} unacknowledged)",
              dist, r.slot, r.count, r.inputs_sent, r.inputs_acked, r.frames_played,
              r.host_messages.in_next, r.host_messages.out_base + r.host_messages.out.size(),
              r.host_messages.out.size());
  REXLOG_INFO("[relay] dist {:08X}: resends {}, dropped packets {}, stalled {:.0f} ms", dist,
              r.resends, r.dropped, r.stalled_ms);
  if (r.socket != kNoSocket) {
    CloseSocket(r.socket);
  }
  r = Relay{};
  g_timing = Timing{};
  // The next match learns its host from its own game link.
  Nhl3on3ClearGamePeers();
}

bool Carries(uint32_t dist) {
  std::lock_guard lock(g_mutex);
  return dist && dist == g_relay.dist;
}

int32_t SendInput(const uint8_t* base, uint32_t dist, uint8_t type, uint32_t data,
                  uint32_t length) {
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  if (dist != r.dist) {
    return 0;
  }
  Input in;
  in.type = type;
  if (data && length) {
    length = std::min(length, kMaxInput);
    in.data.assign(base + data, base + data + length);
  }
  ++r.inputs_sent;
  const auto now = Clock::now();
  if (REXCVAR_GET(relay_timing)) {
    g_timing.sent[r.inputs_sent % kTimingSlots] = now;
  }
  const auto rate = std::chrono::milliseconds(r.rate_ms);
  // Keep the cadence across small scheduling delays, but don't burst to catch
  // up after a long stall.
  r.next_input = (now - r.next_input < rate ? r.next_input : now) + rate;
  if (r.inputs_sent == 1 || r.inputs_sent % 600 == 0) {
    REXLOG_INFO("[relay] input {} type {} len {} (played {})", r.inputs_sent, type,
                in.data.size(), r.frames_played);
  }
  if (r.host) {
    r.clients[r.slot].pending.push_back(std::move(in));
  } else {
    // The send window for this input opened when frame (input - lead) was
    // played; any delay before the game sent it counts against the lead too.
    auto opened = now;
    if (r.inputs_sent > r.lead) {
      const auto played = r.played_at[(r.inputs_sent - r.lead) % kPlayedSlots];
      if (played != Clock::time_point{} && played <= now) {
        opened = played;
      }
    }
    r.unacked.push_back(std::move(in));
    r.unacked_times.push_back(opened);
    r.last_send = {};
  }
  Pump();
  return 1;
}

int32_t QueryFrame(uint8_t* base, uint32_t dist, uint32_t types, uint32_t datas,
                   uint32_t lengths) {
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  if (dist != r.dist) {
    return 0;
  }
  Pump();
  const auto now = Clock::now();
  if (r.frames_played >= FramesEnd()) {
    TimeEmptyQuery();
    NoteStall(now);
    return 0;
  }
  EndStall(now);
  const Frame& frame = r.frames[r.frames_played - r.frames_base];
  for (uint32_t s = 0; s < r.count; ++s) {
    const Input& in = frame[s];
    // The game's per-slot buffers hold kMaxInput bytes.
    const uint32_t length =
        in.type == 1 ? 0 : std::min<uint32_t>(uint32_t(in.data.size()), kMaxInput);
    if (types) {
      base[types + s] = in.type;
    }
    if (lengths) {
      Store32(base + lengths + s * 4, length);
    }
    const uint32_t dest = datas ? Load32(base + datas + s * 4) : 0;
    if (dest && length) {
      std::memcpy(base + dest, in.data.data(), length);
    }
  }
  HashFrame(frame);
  ++r.frames_played;
  r.played_at[r.frames_played % kPlayedSlots] = now;
  TimeFramePlayed(r.frames_played, FramesEnd() - r.frames_played);
  if (r.frames_played % kHashEvery == 0) {
    if (r.host) {
      RecordHash(r.slot, r.frames_played, r.hash);
    } else {
      SendHash(r.frames_played, r.hash);
    }
  }
  if (!r.host) {
    while (r.frames_base < r.frames_played && !r.frames.empty()) {
      r.frames.pop_front();
      ++r.frames_base;
    }
  }
  return int32_t(r.frames_played);
}

int32_t SendWait(uint32_t dist, uint32_t rate_ms, uint32_t& frames_ready) {
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  frames_ready = 0;
  if (dist != r.dist) {
    return 0;
  }
  Pump();
  frames_ready = FramesEnd() - std::min(r.frames_played, FramesEnd());
  if (!rate_ms) {
    rate_ms = kDefaultRateMs;
  }
  if (rate_ms != r.rate_ms) {
    REXLOG_INFO("[relay] input rate {} ms", rate_ms);
    r.rate_ms = rate_ms;
  }
  if (r.inputs_sent - r.frames_played >= (r.host ? kWindow : r.lead)) {
    return r.host ? 10 : int32_t(std::max<uint32_t>(r.rate_ms / 4, 1));
  }
  const auto now = Clock::now();
  if (now >= r.next_input) {
    return 0;
  }
  return int32_t(std::chrono::ceil<std::chrono::milliseconds>(r.next_input - now).count());
}

int32_t SendControl(const uint8_t* base, uint32_t dist, uint8_t type, uint32_t data,
                    uint32_t length) {
  std::lock_guard lock(g_mutex);
  Relay& r = g_relay;
  if (dist != r.dist) {
    return 0;
  }
  Input in;
  in.type = type;
  if (data && length) {
    length = std::min(length, kMaxMessage);
    in.data.assign(base + data, base + data + length);
  }
  REXLOG_INFO("[relay] send message type {} len {}", type, in.data.size());
  if (r.host) {
    for (uint32_t s = 0; s < r.count; ++s) {
      if (s != r.slot) {
        QueueMessage(s, in);
        r.clients[s].messages.last_send = {};
      }
    }
  } else {
    r.host_messages.out.push_back(std::move(in));
    r.host_messages.last_send = {};
  }
  Pump();
  return 2;
}

void DeliverMessages(const PPCContext& ctx, uint8_t* base) {
  std::deque<Input> inbox;
  {
    std::lock_guard lock(g_mutex);
    if (!g_relay.dist) {
      return;
    }
    Pump();
    inbox.swap(g_relay.inbox);
  }
  for (const Input& in : inbox) {
    // "gamedata" + type + payload, on the guest stack below the caller's frame.
    const uint32_t length = 9 + uint32_t(in.data.size());
    const uint32_t buffer = (ctx.r1.u32 - 0x400 - length) & ~0xFu;
    std::memcpy(base + buffer, "gamedata", 8);
    base[buffer + 8] = in.type;
    if (!in.data.empty()) {
      std::memcpy(base + buffer + 9, in.data.data(), in.data.size());
    }
    PPCContext call = ctx;
    call.r1.u64 = buffer - 0x100;
    call.r3.u64 = 0;
    call.r4.u64 = 0;
    call.r5.u64 = 0;
    call.r6.u64 = buffer;
    call.r7.u64 = length;
    sub_82515070(call, base);
  }
}

}  // namespace nhl3on3::lockstep_relay
