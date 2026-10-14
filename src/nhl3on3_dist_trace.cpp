// Traces the lockstep input stream (DirtySDK NetGameDist) of an online match.
//
// The dist object keeps 64-entry input rings and, in multi mode, exchanges
// frames holding one input blob per slot:
//   +1124 local slot index    +1128 slot count    +1136 multi mode flag
//   +1068 frames consumed     +2360 last local packet (type byte + data)
// sub_827582E8(dist, data, length)               local input, single buffer
// sub_82758F40(dist, types, datas, lengths, n)   local input, per-slot arrays
// sub_82758BE8(dist, types, datas, lengths)      next frame, per-slot arrays;
//                                                returns the frame number
// Slot types: 0/2/3 carry data, 1 means "unchanged" (no data).

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "nhl3on3_lockstep_relay.h"

namespace relay = nhl3on3::lockstep_relay;

REXCVAR_DECLARE(int32_t, team_play);

REXCVAR_DEFINE_INT32(dist_trace, 0, "NHL3on3/Debug",
                     "Log every Nth lockstep input frame sent and received (0 = off)")
    .range(0, 100000);

REXCVAR_DEFINE_INT32(dist_trace_bytes, 48, "NHL3on3/Debug",
                     "Bytes of each input blob to dump in the dist trace")
    .range(0, 256);

REXCVAR_DEFINE_INT32(dist_mode, 0, "NHL3on3/Debug",
                     "Force the match's lockstep mode when the dist module is built "
                     "(3 = 12-slot multi-console module, 6 = normal 1v1; 0 = game's choice)")
    .range(0, 16);

extern "C" {
REX_FUNC(__imp__sub_827582E8);
REX_FUNC(__imp__sub_82758F40);
REX_FUNC(__imp__sub_82758BE8);
REX_FUNC(__imp__sub_82519D08);
}

namespace {

constexpr int kMaxSlots = 12;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

std::string Hex(const uint8_t* base, uint32_t address, uint32_t length) {
  const uint32_t shown = std::min<uint32_t>(length, uint32_t(REXCVAR_GET(dist_trace_bytes)));
  std::string out;
  for (uint32_t i = 0; i < shown; ++i) {
    fmt::format_to(std::back_inserter(out), "{:02X}", base[address + i]);
  }
  if (shown < length) {
    out += "..";
  }
  return out;
}

int SlotCount(const uint8_t* base, uint32_t dist) {
  const int count = int(Load32(base, dist + 1128));
  return count < 1 ? 1 : count > kMaxSlots ? kMaxSlots : count;
}

void LogMode(const uint8_t* base, uint32_t dist) {
  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  const uint32_t local = Load32(base, dist + 1124);
  const uint32_t slots = Load32(base, dist + 1128);
  const uint32_t multi = Load32(base, dist + 1136);
  const uint64_t key = (uint64_t(dist) << 32) ^ (uint64_t(local) << 16) ^ (slots << 1) ^ !!multi;
  std::lock_guard lock(mutex);
  if (seen.insert(key).second) {
    REXLOG_INFO("[dist] {:08X} mode: local slot {} of {} slots, multi {:X}", dist, local, slots,
                multi);
  }
}

bool ShouldLog(uint32_t frame) {
  const int every = REXCVAR_GET(dist_trace);
  return every > 0 && frame % uint32_t(every) == 0;
}

}  // namespace

// Match start: (module slot, peer dist, mode). Builds the game's one
// NETWORK::DistModule around the first peer's dist (mode 3 selects the 12-slot
// multi module); later peers find the slot taken and are ignored.
extern "C" REX_FUNC(sub_82519D08) {
  const uint32_t slot = ctx.r3.u32, dist = ctx.r4.u32;
  const uint32_t game_mode = ctx.r5.u32;
  if (const int32_t forced = REXCVAR_GET(dist_mode); forced > 0) {
    ctx.r5.u32 = uint32_t(forced);
    Store32(base, 0x82FB600C, uint32_t(forced));
  }
  const uint32_t mode = ctx.r5.u32;
  const uint32_t existing = slot ? Load32(base, slot) : 0;
  __imp__sub_82519D08(ctx, base);
  if (REXCVAR_GET(dist_trace) > 0 || REXCVAR_GET(dist_mode) > 0) {
    const uint32_t users = Load32(base, 0x8317D498);
    REXLOG_INFO("[dist] module for dist {:08X} mode {} (game chose {}, settings {:02X} {:02X} "
                "{:02X}, users+168 {}): {}; dist slot {} of {} multi {:X}",
                dist, int32_t(mode), int32_t(game_mode), base[0x82FB6008], base[0x82FB6009],
                base[0x82FB600A], users ? int32_t(Load32(base, users + 168)) : -1,
                existing ? fmt::format("ignored, module {:08X} exists", existing)
                         : fmt::format("created {:08X}", slot ? Load32(base, slot) : 0),
                int32_t(Load32(base, dist + 1124)), int32_t(Load32(base, dist + 1128)),
                Load32(base, dist + 1136));
  }
}

std::string Nhl3on3GuestBacktrace(const PPCContext& ctx, const uint8_t* base);

namespace {

// The current game mode object (*0x8317D494) runs a small state machine:
// +64 current, +68 requested, +72 previous. Any non-zero state during an
// online match makes the flow post substate 11 ("opponent has left").
std::array<uint32_t, 2> GameModeState(const uint8_t* base) {
  const uint32_t mode = Load32(base, 0x8317D494);
  if (!mode) {
    return {~0u, ~0u};
  }
  return {Load32(base, mode + 64), Load32(base, mode + 68)};
}

void TraceGameModeState(const char* name, const PPCContext& entry, const uint8_t* base,
                        const std::array<uint32_t, 2>& before) {
  const auto after = GameModeState(base);
  if (after == before || after[0] == ~0u || before[0] == ~0u) {
    return;
  }
  REXLOG_INFO("[mode] {} changed game mode state {}/{} -> {}/{} (current/requested); "
              "called from {:08X}",
              name, int32_t(before[0]), int32_t(before[1]), int32_t(after[0]), int32_t(after[1]),
              uint32_t(entry.lr));
}

}  // namespace

#define GAME_MODE_STATE_WRITER(name)                                           \
  extern "C" REX_FUNC(__imp__##name);                                          \
  extern "C" REX_FUNC(name) {                                                  \
    if (REXCVAR_GET(dist_trace) <= 0) {                                        \
      __imp__##name(ctx, base);                                                \
      return;                                                                  \
    }                                                                          \
    const PPCContext entry = ctx;                                              \
    const auto before = GameModeState(base);                                   \
    __imp__##name(ctx, base);                                                  \
    TraceGameModeState(#name, entry, base, before);                            \
  }

namespace {

bool IsGuestStack(uint32_t address) {
  return address >= 0x70000000 && address < 0x80000000;
}

// Logs a NetGameDist call whenever its arguments, result or pointed-to outputs
// differ from the previous call on the same object (at most 40 lines each).
void TraceDistCall(const char* name, const uint8_t* base, uint32_t self, uint32_t r4,
                   uint32_t r5, uint32_t r6, uint32_t result) {
  static std::mutex mutex;
  static std::unordered_map<uint64_t, std::pair<std::array<uint32_t, 6>, int>> last;
  const uint32_t out4 = IsGuestStack(r4) ? Load32(base, r4) : 0;
  const uint32_t out5 = IsGuestStack(r5) ? Load32(base, r5) : 0;
  const std::array<uint32_t, 6> now{r4, r5, r6, result, out4, out5};
  const uint64_t key = (uint64_t(self) << 32) ^ std::hash<std::string_view>{}(name);
  std::lock_guard lock(mutex);
  auto& [seen, count] = last[key];
  if (seen == now || count >= 40) {
    return;
  }
  seen = now;
  ++count;
  REXLOG_INFO("[dist] {}({:08X}, {:08X}, {:08X}, {:08X}) -> {:08X}{}", name, self, r4, r5, r6,
              result,
              IsGuestStack(r4) || IsGuestStack(r5)
                  ? fmt::format("  [r4]={:08X} [r5]={:08X}", out4, out5)
                  : std::string());
}

}  // namespace

#define DIST_CALL(name)                                                        \
  extern "C" REX_FUNC(__imp__##name);                                          \
  extern "C" REX_FUNC(name) {                                                  \
    const uint32_t self = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32,        \
                   r6 = ctx.r6.u32;                                            \
    __imp__##name(ctx, base);                                                  \
    if (REXCVAR_GET(dist_trace) > 0) {                                         \
      TraceDistCall(#name, base, self, r4, r5, r6, ctx.r3.u32);                \
    }                                                                          \
  }

DIST_CALL(sub_82757E48)
DIST_CALL(sub_82757EC0)
DIST_CALL(sub_82757FC8)
DIST_CALL(sub_82758058)
DIST_CALL(sub_82758098)
DIST_CALL(sub_827580B8)
// Dist destroy (owner, dist); logged with who tore the match down.
extern "C" REX_FUNC(__imp__sub_82758228);
extern "C" REX_FUNC(sub_82758228) {
  REXLOG_INFO("[dist] destroy dist {:08X} (owner {:08X}); backtrace: {}", ctx.r4.u32,
              ctx.r3.u32, Nhl3on3GuestBacktrace(ctx, base));
  __imp__sub_82758228(ctx, base);
}

// Multi setup (dist, local slot, slot count).
extern "C" REX_FUNC(__imp__sub_827582D8);
extern "C" REX_FUNC(sub_827582D8) {
  const uint32_t self = ctx.r3.u32, slot = ctx.r4.u32, count = ctx.r5.u32;
  __imp__sub_827582D8(ctx, base);
  if (REXCVAR_GET(dist_trace) > 0) {
    TraceDistCall("sub_827582D8", base, self, slot, count, 0, ctx.r3.u32);
  }
  relay::OnMultiSetup(self, slot, count);
}

// The dist's input rate in ms (NetGameDistInputRate); 0 until the game sets it.
constexpr uint32_t kDistInputRate = 1080;

// Send window (dist, int* wait ms, int* queued); the game sends when wait <= 0.
extern "C" REX_FUNC(__imp__sub_827591B0);
extern "C" REX_FUNC(sub_827591B0) {
  const uint32_t self = ctx.r3.u32, wait = ctx.r4.u32, queued = ctx.r5.u32;
  if (relay::Carries(self)) {
    uint32_t ready = 0;
    const int32_t ms = relay::SendWait(self, Load32(base, self + kDistInputRate), ready);
    if (wait) {
      Store32(base, wait, uint32_t(ms));
    }
    if (queued) {
      Store32(base, queued, ready);
    }
    relay::DeliverMessages(ctx, base);
    ctx.r3.u64 = uint32_t(ms);
    return;
  }
  __imp__sub_827591B0(ctx, base);
  if (REXCVAR_GET(dist_trace) > 0) {
    TraceDistCall("sub_827591B0", base, self, wait, queued, 0, ctx.r3.u32);
  }
}

DIST_CALL(sub_82758338)
DIST_CALL(sub_82758348)  // multi reset
DIST_CALL(sub_82758370)

// Dist update: pumps the link, where stream messages are normally received.
extern "C" REX_FUNC(__imp__sub_827586A0);
extern "C" REX_FUNC(sub_827586A0) {
  const uint32_t self = ctx.r3.u32;
  const PPCContext entry = ctx;
  __imp__sub_827586A0(ctx, base);
  if (relay::Carries(self)) {
    relay::DeliverMessages(entry, base);
  }
  if (REXCVAR_GET(dist_trace) > 0) {
    TraceDistCall("sub_827586A0", base, self, entry.r4.u32, entry.r5.u32, entry.r6.u32,
                  ctx.r3.u32);
  }
}

// Match module control message send (module, {u32 type, u32 length, data*}):
// "gamedata" + type + payload on the dist's 'osdk' stream. Returns 2 if sent.
extern "C" REX_FUNC(__imp__sub_82512398);
extern "C" REX_FUNC(sub_82512398) {
  const uint32_t module = ctx.r3.u32, message = ctx.r4.u32;
  const uint32_t dist = module ? Load32(base, module + 4) : 0;
  if (message && relay::Carries(dist)) {
    ctx.r3.u64 = uint32_t(relay::SendControl(base, dist, uint8_t(Load32(base, message)),
                                             Load32(base, message + 8),
                                             Load32(base, message + 4)));
    return;
  }
  __imp__sub_82512398(ctx, base);
}
DIST_CALL(sub_82758A18)
DIST_CALL(sub_82758A78)
DIST_CALL(sub_82758AF8)
DIST_CALL(sub_82758B00)
DIST_CALL(sub_82758B10)
DIST_CALL(sub_82759460)
// Match start: arms the input poll (*0x8317A724 +8) through the network game
// object at +8, which must exist.
extern "C" REX_FUNC(__imp__sub_8251B280);
extern "C" REX_FUNC(sub_8251B280) {
  const uint32_t self = ctx.r3.u32;
  if (REXCVAR_GET(dist_trace) > 0 && self) {
    REXLOG_INFO("[dist] match start: net {:08X} game {:08X} +16 {:08X}", self,
                Load32(base, self + 8), Load32(base, self + 16));
  }
  __imp__sub_8251B280(ctx, base);
  if (REXCVAR_GET(dist_trace) > 0) {
    const uint32_t poll = Load32(base, 0x8317A724);
    REXLOG_INFO("[dist] match start done: input poll {:08X} armed {}", poll,
                poll ? base[poll + 8] : 0);
  }
}

namespace {

// The network game object (*0x8317A720 +8) +68: set when lockstep is given up
// and the match falls back to a local timer instead of the dist module.
uint8_t LockstepFallback(const uint8_t* base) {
  const uint32_t net = Load32(base, 0x8317A720);
  const uint32_t game = net ? Load32(base, net + 8) : 0;
  return game ? base[game + 68] : 0xFF;
}

}  // namespace

#define LOCKSTEP_FALLBACK_WRITER(name)                                         \
  extern "C" REX_FUNC(__imp__##name);                                          \
  extern "C" REX_FUNC(name) {                                                  \
    if (REXCVAR_GET(dist_trace) <= 0) {                                        \
      __imp__##name(ctx, base);                                                \
      return;                                                                  \
    }                                                                          \
    const PPCContext entry = ctx;                                              \
    const uint8_t before = LockstepFallback(base);                             \
    __imp__##name(ctx, base);                                                  \
    const uint8_t after = LockstepFallback(base);                              \
    if (after != before) {                                                     \
      REXLOG_INFO("[dist] {} changed lockstep fallback {} -> {}; backtrace: {}", \
                  #name, before, after, Nhl3on3GuestBacktrace(entry, base));   \
    }                                                                          \
  }

LOCKSTEP_FALLBACK_WRITER(sub_82517B70)
LOCKSTEP_FALLBACK_WRITER(sub_8251E890)
LOCKSTEP_FALLBACK_WRITER(sub_825139E8)
LOCKSTEP_FALLBACK_WRITER(sub_8251FA30)
LOCKSTEP_FALLBACK_WRITER(sub_8251D8C8)
LOCKSTEP_FALLBACK_WRITER(sub_8251F068)

namespace {

// Network manager match setup status (*0x8317A720 +28); 9 = failed, which
// creates the network game in local-timer fallback mode.
uint32_t NetSetupStatus(const uint8_t* base) {
  const uint32_t net = Load32(base, 0x8317A720);
  return net ? Load32(base, net + 28) : ~0u;
}

}  // namespace

#define NET_SETUP_STATUS_WRITER(name)                                          \
  extern "C" REX_FUNC(__imp__##name);                                          \
  extern "C" REX_FUNC(name) {                                                  \
    if (REXCVAR_GET(dist_trace) <= 0) {                                        \
      __imp__##name(ctx, base);                                                \
      return;                                                                  \
    }                                                                          \
    const PPCContext entry = ctx;                                              \
    const uint32_t before = NetSetupStatus(base);                              \
    __imp__##name(ctx, base);                                                  \
    const uint32_t after = NetSetupStatus(base);                               \
    if (after != before) {                                                     \
      REXLOG_INFO("[dist] {} changed net setup status {} -> {}; backtrace: {}", \
                  #name, int32_t(before), int32_t(after),                      \
                  Nhl3on3GuestBacktrace(entry, base));                         \
    }                                                                          \
  }

// Ready barrier: waits until barrier+8 (ready peers) reaches r4 (players - 1).
// barrier+4 points at a 12-slot per-peer table; session+8 is the link module.
extern "C" REX_FUNC(__imp__sub_8251CDF0);
extern "C" REX_FUNC(sub_8251CDF0) {
  relay::DeliverMessages(ctx, base);
  if (REXCVAR_GET(dist_trace) <= 0) {
    __imp__sub_8251CDF0(ctx, base);
    return;
  }
  const uint32_t barrier = ctx.r3.u32;
  const int32_t target = ctx.r4.s32;
  __imp__sub_8251CDF0(ctx, base);
  const uint32_t session = Load32(base, 0x8317A710);
  const uint32_t module = session ? Load32(base, session + 8) : 0;
  const uint32_t table = Load32(base, barrier + 4);
  std::string slots;
  for (int i = 0; table && i < 12; ++i) {
    slots += fmt::format("{}{:X}", i ? " " : "", Load32(base, table + i * 4));
  }
  const std::string state = fmt::format(
      "count {} target {} result {} players {} link {:08X} connected {} slots [{}]",
      int32_t(Load32(base, barrier + 8)), target, ctx.r3.s32,
      session ? int8_t(base[session + 224]) : -1, module,
      module ? base[module + 12] : 0xFF, slots);
  static std::string last;
  if (state != last) {
    last = state;
    REXLOG_INFO("[dist] ready barrier {:08X}: {}", barrier, state);
  }
}

namespace {

void LogOnChangeKeyed(const char* what, uint64_t key, const std::string& text) {
  static std::mutex mutex;
  static std::unordered_map<uint64_t, std::string> last;
  std::lock_guard lock(mutex);
  auto& seen = last[key ^ std::hash<std::string_view>{}(what)];
  if (seen != text) {
    seen = text;
    REXLOG_INFO("[dist] {} {}", what, text);
  }
}

}  // namespace

// Queues a control message (queue, {u32 type, u32 length, data*}).
extern "C" REX_FUNC(__imp__sub_8251CBA8);
extern "C" REX_FUNC(sub_8251CBA8) {
  const uint32_t message = ctx.r4.u32;
  if (REXCVAR_GET(dist_trace) > 0 && message) {
    const uint32_t length = Load32(base, message + 4), data = Load32(base, message + 8);
    REXLOG_INFO("[dist] queue message type {} len {} {}", int32_t(Load32(base, message)),
                length, data ? Hex(base, data, length) : "-");
  }
  __imp__sub_8251CBA8(ctx, base);
}

// Sends one queued control message (module, message); 2 = sent.
extern "C" REX_FUNC(__imp__sub_82514FE8);
extern "C" REX_FUNC(sub_82514FE8) {
  const uint32_t message = ctx.r4.u32;
  const uint32_t type = message ? Load32(base, message) : ~0u;
  __imp__sub_82514FE8(ctx, base);
  if (REXCVAR_GET(dist_trace) > 0) {
    LogOnChangeKeyed("send queued message", 0,
                     fmt::format("type {} -> {}", int32_t(type), int32_t(ctx.r3.u32)));
  }
}

namespace {

// Match mode global (+4: 3 = EASHL multi-console) and the session member
// attribute keys the roster step uses: side, lineup slot, pad.
constexpr uint32_t kMatchMode = 0x82FB6008;
constexpr uint32_t kSideKey = 0x82FB559C;
constexpr uint32_t kPadKey = 0x82FB55A8;

// Mode 3 expects each member's side to come from lobby team selection, which
// team play skips; the roster step then waits on it until it times out.
// Members are visited in session order on every console, so sides alternate
// along it (host 0, next 1, ...), set just before the step writes each pad.
struct SideAssign {
  bool active = false;
  uint32_t next = 0;
};
SideAssign g_side_assign;

}  // namespace

// Mode setup roster step (status +16); true once every session member is known.
extern "C" REX_FUNC(__imp__sub_8251D0D0);
extern "C" REX_FUNC(sub_8251D0D0) {
  const uint32_t self = ctx.r3.u32;
  const uint8_t sent_before = base[self + 4];
  g_side_assign = {};
  g_side_assign.active =
      !sent_before && REXCVAR_GET(team_play) >= 3 && Load32(base, kMatchMode + 4) == 3;
  __imp__sub_8251D0D0(ctx, base);
  g_side_assign.active = false;
  if (REXCVAR_GET(dist_trace) > 0) {
    LogOnChangeKeyed("roster step", self,
                     fmt::format("{:08X}: sent {} -> {}, done {}", self, sent_before,
                                 base[self + 4], ctx.r3.u32 & 0xFF));
  }
}

// Roster lookup (roster, member id, -1) -> index or -1.
extern "C" REX_FUNC(__imp__sub_82512C20);
extern "C" REX_FUNC(sub_82512C20) {
  const uint32_t self = ctx.r3.u32, id = ctx.r4.u32, extra = ctx.r5.u32;
  __imp__sub_82512C20(ctx, base);
  if (REXCVAR_GET(dist_trace) > 0) {
    const uint32_t vtable = id ? Load32(base, id) : 0;
    const uint32_t key = Load32(base, 0x82FB559C);
    std::string key_name;
    for (uint32_t i = 0; key && i < 32 && base[key + i]; ++i) {
      key_name += char(base[key + i]);
    }
    LogOnChangeKeyed("roster lookup", (uint64_t(self) << 32) ^ id ^ (uint64_t(extra) << 20),
                     fmt::format("{:08X} member {:08X} '{}' (default {}) -> {}; member vtable "
                                 "{:08X} get {:08X} set {:08X}",
                                 self, id, key_name, int32_t(extra), int32_t(ctx.r3.u32), vtable,
                                 vtable ? Load32(base, vtable + 4) : 0,
                                 vtable ? Load32(base, vtable + 52) : 0));
  }
}

namespace {

constexpr uint32_t kSessionMemberVtable = 0x821DED80;

std::string GuestString(const uint8_t* base, uint32_t address) {
  std::string text;
  for (uint32_t i = 0; address && i < 32 && base[address + i]; ++i) {
    text += char(base[address + i]);
  }
  return text;
}

}  // namespace

// Session member attribute get (member, key, default) -> value.
extern "C" REX_FUNC(__imp__sub_82B80738);
extern "C" REX_FUNC(sub_82B80738) {
  const uint32_t self = ctx.r3.u32, key = ctx.r4.u32, fallback = ctx.r5.u32;
  __imp__sub_82B80738(ctx, base);
  if (REXCVAR_GET(dist_trace) > 0 && self && Load32(base, self) == kSessionMemberVtable) {
    LogOnChangeKeyed("member get", (uint64_t(self) << 32) ^ key ^ (uint64_t(fallback) << 16),
                     fmt::format("{:08X} '{}' ({:08X}, default {}) -> {}", self,
                                 GuestString(base, key), key, int32_t(fallback),
                                 int32_t(ctx.r3.u32)));
  }
}

// Session member attribute set (member, key, value).
extern "C" REX_FUNC(__imp__sub_82B80820);
extern "C" REX_FUNC(sub_82B80820) {
  const uint32_t self = ctx.r3.u32, key = ctx.r4.u32, value = ctx.r5.u32;
  if (g_side_assign.active && key && key == Load32(base, kPadKey)) {
    const uint32_t side = g_side_assign.next++ % 2;
    REXLOG_INFO("[dist] team play: member {:08X} (#{}) on side {}", self,
                g_side_assign.next - 1, side);
    PPCContext set = ctx;
    set.r4.u64 = Load32(base, kSideKey);
    set.r5.u64 = side;
    __imp__sub_82B80820(set, base);
  }
  if (REXCVAR_GET(dist_trace) > 0 && self && Load32(base, self) == kSessionMemberVtable) {
    LogOnChangeKeyed("member set", (uint64_t(self) << 32) ^ key,
                     fmt::format("{:08X} owner {:08X} '{}' = {}; backtrace: {}", self,
                                 Load32(base, self + 8), GuestString(base, key), int32_t(value),
                                 Nhl3on3GuestBacktrace(ctx, base)));
  }
  __imp__sub_82B80820(ctx, base);
}

NET_SETUP_STATUS_WRITER(sub_825127A8)
NET_SETUP_STATUS_WRITER(sub_825155A8)
NET_SETUP_STATUS_WRITER(sub_825159E8)
NET_SETUP_STATUS_WRITER(sub_82515B38)
NET_SETUP_STATUS_WRITER(sub_8251ABC0)
NET_SETUP_STATUS_WRITER(sub_8251C018)
NET_SETUP_STATUS_WRITER(sub_8251CC40)
NET_SETUP_STATUS_WRITER(sub_8251D058)
NET_SETUP_STATUS_WRITER(sub_8251DF10)
NET_SETUP_STATUS_WRITER(sub_8251DFC8)
NET_SETUP_STATUS_WRITER(sub_8251E4D8)
NET_SETUP_STATUS_WRITER(sub_8251F3E0)
NET_SETUP_STATUS_WRITER(sub_8251F4C8)
NET_SETUP_STATUS_WRITER(sub_8251F8E0)

DIST_CALL(sub_82517FF8)  // arms the input poll
DIST_CALL(sub_82517CE8)  // per-frame "may send input" poll
DIST_CALL(sub_82517548)
DIST_CALL(sub_82514EF0)  // module: ready to send input
DIST_CALL(sub_825122D0)  // module: link still connected

// Online flow update while the game mode state is non-zero; with a non-zero
// player index (+52 -> +168) and +12 clear it posts flow substate 11.
extern "C" REX_FUNC(__imp__sub_825518A0);
extern "C" REX_FUNC(sub_825518A0) {
  if (REXCVAR_GET(dist_trace) > 0) {
    static std::mutex mutex;
    static std::array<uint32_t, 4> last{};
    const uint32_t self = ctx.r3.u32;
    const uint32_t mode = Load32(base, 0x8317D494);
    const uint32_t users = Load32(base, 0x8317D498);
    const std::array<uint32_t, 4> now{mode ? Load32(base, mode + 64) : ~0u,
                                      users ? Load32(base, users + 168) : ~0u, base[self + 12],
                                      Load32(base, Load32(base, self + 4) + 128)};
    std::lock_guard lock(mutex);
    if (now != last) {
      last = now;
      REXLOG_INFO("[mode] online flow update: state {} player index {} +12 {} +4->+128 {}",
                  int32_t(now[0]), int32_t(now[1]), now[2], now[3]);
    }
  }
  __imp__sub_825518A0(ctx, base);
}

GAME_MODE_STATE_WRITER(sub_82676E18)  // commit requested state
GAME_MODE_STATE_WRITER(sub_82677370)
GAME_MODE_STATE_WRITER(sub_8254BC58)
GAME_MODE_STATE_WRITER(sub_8254E2A0)
GAME_MODE_STATE_WRITER(sub_8254F928)
GAME_MODE_STATE_WRITER(sub_8254FEB0)
GAME_MODE_STATE_WRITER(sub_82550AB0)
GAME_MODE_STATE_WRITER(sub_82551678)
GAME_MODE_STATE_WRITER(sub_82551B00)
GAME_MODE_STATE_WRITER(sub_82551C88)
GAME_MODE_STATE_WRITER(sub_825533D0)
GAME_MODE_STATE_WRITER(sub_82553628)
GAME_MODE_STATE_WRITER(sub_82554FE8)
GAME_MODE_STATE_WRITER(sub_825552C8)
GAME_MODE_STATE_WRITER(sub_825557A8)
GAME_MODE_STATE_WRITER(sub_82555B68)
GAME_MODE_STATE_WRITER(sub_825607C8)
GAME_MODE_STATE_WRITER(sub_8239AEE8)
GAME_MODE_STATE_WRITER(sub_824081D0)
GAME_MODE_STATE_WRITER(sub_82408280)
GAME_MODE_STATE_WRITER(sub_824083B0)
GAME_MODE_STATE_WRITER(sub_8240FAF0)
GAME_MODE_STATE_WRITER(sub_8240FBD0)
GAME_MODE_STATE_WRITER(sub_82696BE0)
GAME_MODE_STATE_WRITER(sub_82699F98)
GAME_MODE_STATE_WRITER(sub_8269A1F8)

extern "C" REX_FUNC(sub_827582E8) {
  const uint32_t dist = ctx.r3.u32, data = ctx.r4.u32, length = ctx.r5.u32;
  if (relay::Carries(dist)) {
    ctx.r3.u64 = uint32_t(relay::SendInput(base, dist, 2, data, length));
    return;
  }
  if (REXCVAR_GET(dist_trace) > 0 && dist) {
    LogMode(base, dist);
    static uint32_t sent = 0;
    if (ShouldLog(sent++)) {
      REXLOG_INFO("[dist] tx1 #{} len {} {}", sent - 1, length,
                  data ? Hex(base, data, length) : "-");
    }
  }
  __imp__sub_827582E8(ctx, base);
}

extern "C" REX_FUNC(sub_82758F40) {
  const uint32_t dist = ctx.r3.u32, types = ctx.r4.u32, datas = ctx.r5.u32,
                 lengths = ctx.r6.u32, count = ctx.r7.u32;
  // The match module sends one blob per call, in slot 0 of the arrays.
  if (relay::Carries(dist) && types && datas && lengths) {
    ctx.r3.u64 = uint32_t(relay::SendInput(base, dist, base[types], Load32(base, datas),
                                           Load32(base, lengths)));
    return;
  }
  if (REXCVAR_GET(dist_trace) > 0 && dist && types && datas && lengths) {
    LogMode(base, dist);
    static uint32_t sent = 0;
    if (ShouldLog(sent++)) {
      const uint32_t local = Load32(base, dist + 1124);
      const uint32_t multi = Load32(base, dist + 1136);
      const uint32_t slot = multi ? local : 0;
      const uint32_t length = Load32(base, lengths + slot * 4);
      const uint32_t data = Load32(base, datas + slot * 4);
      REXLOG_INFO("[dist] txM #{} n {} slot {} type {} len {} {}", sent - 1, count, slot,
                  base[types + slot], length, data ? Hex(base, data, length) : "-");
    }
  }
  __imp__sub_82758F40(ctx, base);
}

extern "C" REX_FUNC(sub_82758BE8) {
  const uint32_t dist = ctx.r3.u32, types = ctx.r4.u32, datas = ctx.r5.u32,
                 lengths = ctx.r6.u32;
  if (relay::Carries(dist)) {
    ctx.r3.u64 = uint32_t(relay::QueryFrame(base, dist, types, datas, lengths));
  } else {
    __imp__sub_82758BE8(ctx, base);
  }
  const int32_t frame = int32_t(ctx.r3.u32);
  if (REXCVAR_GET(dist_trace) <= 0 || frame <= 0 || !dist || !types || !lengths ||
      !ShouldLog(uint32_t(frame))) {
    return;
  }
  LogMode(base, dist);
  std::string line = fmt::format("[dist] rx frame {}", frame);
  const int slots = SlotCount(base, dist);
  for (int i = 0; i < slots; ++i) {
    const uint8_t type = base[types + i];
    const uint32_t length = Load32(base, lengths + i * 4);
    const uint32_t data = datas ? Load32(base, datas + i * 4) : 0;
    fmt::format_to(std::back_inserter(line), " | s{} t{} len {} {}", i, type, length,
                   type != 1 && data && length ? Hex(base, data, length) : "-");
  }
  REXLOG_INFO("{}", line);
}
