// Display options: ultrawide aspect ratio and convenience render-quality presets.
//
// The game always renders into a 1280x720 front buffer. Ultrawide is done
// anamorphically: the camera projection is widened to the target aspect while
// the guest video mode reports that same aspect, so the presenter stretches the
// 1280x720 frame back to correct proportions on a wide display.

#include "nhl3on3_display.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "runtime_export.h"

REXCVAR_DEFINE_STRING(aspect_ratio, "16:9", "NHL3on3/Display",
                      "Output aspect ratio: 16:9, 21:9, 32:9, auto (desktop), or W:H / WxH "
                      "(for example 3440x1440). Wider than 16:9 widens the in-game camera")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(fps_cap, "60", "NHL3on3/Display",
                      "Frame rate cap: 60 (stock), 90, 120, 144, any value 24-240, or "
                      "unlocked. The game paces itself on the guest vblank, which runs at "
                      "this rate")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(log_fps, false, "NHL3on3/Display",
                    "Log the presented frame rate every 5 seconds");

REXCVAR_DEFINE_BOOL(latency_probe, false, "NHL3on3/Display",
                    "Log input and frame pipeline timing every 5 seconds: controller reads "
                    "per frame, how old the newest read is when a frame is submitted, and "
                    "how long the GPU thread takes to process and present each frame");

REXCVAR_DEFINE_STRING(render_quality, "", "NHL3on3/Display",
                      "Internal render scale preset: 1x (720p), 2x (1440p), 3x (2160p). "
                      "Empty leaves draw_resolution_scale_x/y untouched")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

constexpr double kNativeAspect = 16.0 / 9.0;
constexpr int kGuestHeight = 720;

std::atomic<float> g_camera_aspect{0.0f};

bool ParsePair(std::string_view text, double& a, double& b) {
  size_t sep = text.find_first_of(":xX");
  if (sep == std::string_view::npos) {
    return false;
  }
  auto parse = [](std::string_view s, double& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size() && out > 0.0;
  };
  return parse(text.substr(0, sep), a) && parse(text.substr(sep + 1), b);
}

double DesktopAspect() {
#if defined(_WIN32)
  int w = GetSystemMetrics(SM_CXSCREEN);
  int h = GetSystemMetrics(SM_CYSCREEN);
  return (w > 0 && h > 0) ? double(w) / double(h) : 0.0;
#else
  return 0.0;
#endif
}

double ResolveAspect(std::string_view setting) {
  if (setting.empty() || setting == "16:9") {
    return kNativeAspect;
  }
  if (setting == "auto") {
    double desktop = DesktopAspect();
    return desktop > 0.0 ? desktop : kNativeAspect;
  }
  double w = 0.0, h = 0.0;
  if (!ParsePair(setting, w, h)) {
    REXLOG_WARN("aspect_ratio '{}' not understood; using 16:9", setting);
    return kNativeAspect;
  }
  return w / h;
}

void SetIfDefault(std::string_view name, std::string_view value) {
  if (!rex::cvar::HasNonDefaultValue(name)) {
    rex::cvar::SetFlagByName(name, value);
  }
}

}  // namespace

namespace nhl3on3 {

void ConfigureDisplay() {
  const std::string fps = REXCVAR_GET(fps_cap);
  if (fps == "unlocked" || fps == "0") {
    SetIfDefault("vsync", "false");
    REXLOG_INFO("Frame rate: unlocked");
  } else if (!fps.empty() && fps != "60") {
    double rate = 0.0;
    std::from_chars(fps.data(), fps.data() + fps.size(), rate);
    rate = std::clamp(rate, 24.0, 240.0);
    SetIfDefault("video_mode_refresh_rate", std::to_string(rate));
    REXLOG_INFO("Frame rate: capped at {:g}", rate);
  }

  const std::string quality = REXCVAR_GET(render_quality);
  if (!quality.empty()) {
    int scale = std::clamp(quality[0] - '0', 1, 3);
    SetIfDefault("draw_resolution_scale_x", std::to_string(scale));
    SetIfDefault("draw_resolution_scale_y", std::to_string(scale));
  }
  REXLOG_INFO("Display: vsync {}, render scale {}x{}, fullscreen {}",
              rex::cvar::GetFlagByName("vsync"),
              rex::cvar::GetFlagByName("draw_resolution_scale_x"),
              rex::cvar::GetFlagByName("draw_resolution_scale_y"),
              rex::cvar::GetFlagByName("fullscreen"));

  // Narrower-than-16:9 output keeps the stock camera and is letterboxed.
  double aspect = std::clamp(ResolveAspect(REXCVAR_GET(aspect_ratio)), kNativeAspect, 4.0);
  if (aspect - kNativeAspect < 0.01) {
    return;
  }
  g_camera_aspect.store(float(aspect));

  int width = int(std::lround(kGuestHeight * aspect)) & ~1;
  SetIfDefault("video_mode_width", std::to_string(width));
  SetIfDefault("video_mode_height", std::to_string(kGuestHeight));
  REXLOG_INFO("Ultrawide: aspect {:.4f}, guest video mode {}x{}", aspect, width, kGuestHeight);
}

}  // namespace nhl3on3

namespace {

// latency_probe. The GPU thread consumes the guest's command ring buffer and
// writes its read position (in dwords) back to guest memory; a swap has been
// processed, and with no UI overlay presented, once that position passes the
// swap packet VdSwap writes.
namespace probe {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kPhysicalView = 0xA0000000;  // guest view of physical memory
constexpr size_t kMaxPending = 64;

struct Stats {
  uint32_t swaps = 0, reads = 0, aged = 0, gpu = 0;
  double interval_sum = 0, interval_max = 0;
  double age_sum = 0, age_max = 0;  // newest pad read -> swap submitted
  double gpu_sum = 0, gpu_max = 0;  // swap submitted -> GPU thread past it
  uint64_t in_flight_sum = 0;
};

struct Pending {
  Clock::time_point submitted;
  uint64_t done_at;  // `traveled` once the read position is past the packet
};

std::mutex mutex;
std::atomic<uint8_t*> guest_base{nullptr};
uint32_t ring_phys = 0, ring_dwords = 0, rptr_phys = 0;
uint32_t last_rptr = 0;
uint64_t traveled = 0;
bool bad_rptr = false;
std::deque<Pending> pending;
Clock::time_point last_read{}, last_swap{}, window_start{};
Stats stats;

double Ms(Clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

uint32_t ToPhysical(uint32_t addr) {
  if (addr < kPhysicalView) {
    return ~0u;
  }
  return (addr & 0x1FFFFFFF) - (addr >= 0xE0000000 ? 0x1000 : 0);
}

// Caller holds mutex.
void Advance(Clock::time_point now) {
  uint8_t* base = guest_base.load();
  if (!base || !rptr_phys || !ring_dwords || bad_rptr) {
    return;
  }
  const uint8_t* p = base + kPhysicalView + rptr_phys;
  const uint32_t rptr = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                        (uint32_t(p[2]) << 8) | p[3];
  if (rptr >= ring_dwords) {
    bad_rptr = true;
    pending.clear();
    REXLOG_WARN("[latency] GPU read position {} outside the {}-dword ring; GPU timing off",
                rptr, ring_dwords);
    return;
  }
  traveled += (rptr - last_rptr + ring_dwords) % ring_dwords;
  last_rptr = rptr;
  while (!pending.empty() && pending.front().done_at <= traveled) {
    const double ms = Ms(now - pending.front().submitted);
    stats.gpu_sum += ms;
    stats.gpu_max = std::max(stats.gpu_max, ms);
    ++stats.gpu;
    pending.pop_front();
  }
}

void Poll() {
#if defined(_WIN32)
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
  for (;;) {
#if defined(_WIN32)
    if (timer) {
      LARGE_INTEGER due{};
      due.QuadPart = -2500;  // 0.25 ms
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, INFINITE);
    } else {
      Sleep(1);
    }
#else
    std::this_thread::sleep_for(std::chrono::microseconds(250));
#endif
    if (!REXCVAR_GET(latency_probe)) {
      continue;
    }
    std::lock_guard lock(mutex);
    Advance(Clock::now());
  }
}

void Report(Clock::time_point now) {
  const double elapsed = Ms(now - window_start) / 1000.0;
  const Stats& s = stats;
  if (!s.swaps) {
    return;
  }
  const std::string gpu =
      s.gpu ? fmt::format("submit to presented avg {:.1f} max {:.1f} ms, {:.2f} frames in "
                          "flight at submit",
                          s.gpu_sum / s.gpu, s.gpu_max, double(s.in_flight_sum) / s.swaps)
            : std::string("submit to presented unknown");
  REXLOG_INFO("[latency] {:.1f} frames/s (interval avg {:.1f} max {:.1f} ms); {:.2f} pad reads "
              "per frame, newest read avg {:.1f} max {:.1f} ms old at submit; {}",
              s.swaps / elapsed, s.interval_sum / s.swaps, s.interval_max,
              double(s.reads) / s.swaps, s.aged ? s.age_sum / s.aged : 0.0, s.age_max, gpu);
}

void OnSwap(uint8_t* base, uint32_t buffer_ptr) {
  static std::once_flag started;
  guest_base.store(base);
  std::call_once(started, [] { std::thread(Poll).detach(); });
  const auto now = Clock::now();
  std::lock_guard lock(mutex);
  Advance(now);
  if (window_start == Clock::time_point{}) {
    window_start = now;
  }
  Stats& s = stats;
  if (last_swap != Clock::time_point{}) {
    const double interval = Ms(now - last_swap);
    s.interval_sum += interval;
    s.interval_max = std::max(s.interval_max, interval);
  }
  last_swap = now;
  ++s.swaps;
  if (last_read != Clock::time_point{}) {
    const double age = Ms(now - last_read);
    s.age_sum += age;
    s.age_max = std::max(s.age_max, age);
    ++s.aged;
  }
  const uint32_t phys = ToPhysical(buffer_ptr);
  const uint32_t index = (phys - ring_phys) / 4;
  if (ring_dwords && !bad_rptr && phys != ~0u && phys >= ring_phys && index < ring_dwords &&
      pending.size() < kMaxPending) {
    const uint32_t ahead = (index - last_rptr + ring_dwords) % ring_dwords;
    pending.push_back({now, traveled + ahead + 1});
  }
  s.in_flight_sum += pending.size();
  if (Ms(now - window_start) >= 5000.0) {
    Report(now);
    stats = Stats{};
    window_start = now;
  }
}

}  // namespace probe

}  // namespace

namespace nhl3on3 {

void NotePadRead() {
  if (!REXCVAR_GET(latency_probe)) {
    return;
  }
  std::lock_guard lock(probe::mutex);
  probe::last_read = probe::Clock::now();
  ++probe::stats.reads;
}

}  // namespace nhl3on3

// (physical address, size log2): the GPU command ring, 2^(size_log2 + 3) bytes.
extern "C" REX_FUNC(__imp__VdInitializeRingBuffer) {
  static const nhl3on3::GuestFunc original =
      nhl3on3::ResolveRuntimeExport("__imp__VdInitializeRingBuffer");
  {
    std::lock_guard lock(probe::mutex);
    probe::ring_phys = ctx.r3.u32 & 0x1FFFFFFF;
    probe::ring_dwords = (1u << (ctx.r4.u32 + 3)) / 4;
    probe::last_rptr = 0;
    probe::pending.clear();
  }
  REXLOG_INFO("GPU ring buffer at {:08X}, {} dwords", ctx.r3.u32, probe::ring_dwords);
  original(ctx, base);
}

// (physical address, block size log2): where the GPU writes its read position.
extern "C" REX_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
  static const nhl3on3::GuestFunc original =
      nhl3on3::ResolveRuntimeExport("__imp__VdEnableRingBufferRPtrWriteBack");
  {
    std::lock_guard lock(probe::mutex);
    probe::rptr_phys = ctx.r3.u32 & 0x1FFFFFFF;
  }
  REXLOG_INFO("GPU read position write-back at {:08X}", ctx.r3.u32);
  original(ctx, base);
}

// (swap packet address in the ring, ...)
extern "C" REX_FUNC(__imp__VdSwap) {
  static const nhl3on3::GuestFunc original = nhl3on3::ResolveRuntimeExport("__imp__VdSwap");
  {  // TEMP debug
    static int trap_at = getenv("NHL3ON3_TRAP_SWAP") ? atoi(getenv("NHL3ON3_TRAP_SWAP")) : 0;
    static int swaps = 0;
    if (trap_at && ++swaps == trap_at) {
      __builtin_debugtrap();
    }
  }
  if (REXCVAR_GET(latency_probe)) {
    probe::OnSwap(base, ctx.r3.u32);
  }
  if (REXCVAR_GET(log_fps)) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point window_start = Clock::now();
    static uint32_t frames = 0;
    ++frames;
    const double elapsed = std::chrono::duration<double>(Clock::now() - window_start).count();
    if (elapsed >= 5.0) {
      REXLOG_INFO("FPS: {:.1f}", frames / elapsed);
      frames = 0;
      window_start = Clock::now();
    }
  }
  original(ctx, base);
}

// sub_82B76100 loads 16:9 (or 4:3 on non-widescreen modes) into f13 just before
// deriving the camera projection from it.
void Nhl3on3CameraAspectHook(PPCRegister& f13) {
  float aspect = g_camera_aspect.load(std::memory_order_relaxed);
  if (aspect > 0.0f && std::fabs(float(f13.f64) - float(kNativeAspect)) < 0.001f) {
    f13.f64 = double(aspect);
  }
}

void Nhl3on3ScreenWidthHook(PPCRegister& f0) {
  const float aspect = g_camera_aspect.load(std::memory_order_relaxed);
  if (aspect > 0.0f) {
    f0.f64 = double(float(f0.f64 * aspect / kNativeAspect));
    REXLOG_INFO("Ultrawide: screen width {:.0f}", f0.f64);
  }
}
