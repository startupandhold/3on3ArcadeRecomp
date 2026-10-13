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

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_INT32(dist_trace, 0, "NHL3on3/Debug",
                     "Log every Nth lockstep input frame sent and received (0 = off)")
    .range(0, 100000);

REXCVAR_DEFINE_INT32(dist_trace_bytes, 48, "NHL3on3/Debug",
                     "Bytes of each input blob to dump in the dist trace")
    .range(0, 256);

extern "C" {
REX_FUNC(__imp__sub_827582E8);
REX_FUNC(__imp__sub_82758F40);
REX_FUNC(__imp__sub_82758BE8);
}

namespace {

constexpr int kMaxSlots = 12;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
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

extern "C" REX_FUNC(sub_827582E8) {
  const uint32_t dist = ctx.r3.u32, data = ctx.r4.u32, length = ctx.r5.u32;
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
  __imp__sub_82758BE8(ctx, base);
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
