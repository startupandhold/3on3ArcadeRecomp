// Display options: ultrawide / portrait aspect ratio and convenience
// render-quality presets.
//
// The game always renders into a 1280x720 front buffer. Other aspects are done
// anamorphically: the camera projection is changed to the target aspect and
// the presenter stretches the 1280x720 frame to the display, which restores
// correct proportions (wider for ultrawide, taller for portrait).

#include "nhl3on3_display.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
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
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "runtime_export.h"

REXCVAR_DEFINE_STRING(aspect_ratio, "16:9", "NHL3on3/Display",
                      "Output aspect ratio: 16:9, 21:9, 32:9, 9:16, 9:21, auto (desktop), or "
                      "W:H / WxH (for example 3440x1440). Wider than 16:9 widens the in-game "
                      "camera; narrower (portrait) makes it taller")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(ultrawide_fov, "hor+", "NHL3on3/Display",
                      "How ultrawide widens the 3D view: hor+ (same height, more to the "
                      "sides) or vert- (same width, less above and below). Portrait keeps "
                      "the width and shows more above and below");

REXCVAR_DEFINE_STRING(portrait_camera, "topdown", "NHL3on3/Display",
                      "Match camera with a portrait aspect_ratio: topdown (fixed, looking "
                      "down at the ice) or game (the stock camera)");

REXCVAR_DEFINE_STRING(topdown_direction, "auto", "NHL3on3/Display",
                      "Which end of the rink is at the top of the top-down camera: auto "
                      "(follows the game camera's side of the rink) or reverse (the "
                      "opposite end)");

REXCVAR_DEFINE_DOUBLE(topdown_tilt, 12.0, "NHL3on3/Display",
                      "Top-down camera tilt away from straight down, in degrees");

REXCVAR_DEFINE_DOUBLE(topdown_zoom, 1.0, "NHL3on3/Display",
                      "Top-down camera distance, relative to the stock camera's at the "
                      "opening face-off (smaller is closer)");

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
    if (scale > 1) {
      // Scaled matches drop to ~25 FPS on an RTX 3080 with the host render
      // target path, 2x MSAA and memexport readback; this set holds 60.
#if defined(_WIN32)
      SetIfDefault("render_target_path_d3d12", "rov");
#endif
      SetIfDefault("native_2x_msaa", "false");
      SetIfDefault("readback_memexport", "false");
    }
  }
  REXLOG_INFO("Display: vsync {}, render scale {}x{}, fullscreen {}, 2x MSAA {}, "
              "memexport readback {}",
              rex::cvar::GetFlagByName("vsync"),
              rex::cvar::GetFlagByName("draw_resolution_scale_x"),
              rex::cvar::GetFlagByName("draw_resolution_scale_y"),
              rex::cvar::GetFlagByName("fullscreen"),
              rex::cvar::GetFlagByName("native_2x_msaa"),
              rex::cvar::GetFlagByName("readback_memexport"));

  double aspect = std::clamp(ResolveAspect(REXCVAR_GET(aspect_ratio)), 0.25, 4.0);
  if (std::fabs(aspect - kNativeAspect) < 0.01) {
    return;
  }
  g_camera_aspect.store(float(aspect));

  // The guest video mode stays 16:9 and the presenter stretches the frame to
  // fill a window of the target aspect instead of letterboxing it. A wider
  // video mode loses UI text (glyphs drop out of names, scores and menus); one
  // narrower than 4:3 switches the game to its 4:3 layouts. The video mode
  // must be set: left at its default the SDK takes it from the window size.
  // 1282, not 1280: the SDK only counts a value that differs from the default.
  const bool wide = aspect > kNativeAspect;
  const int window_height = wide ? kGuestHeight : 1280;
  const int window_width = int(std::lround(window_height * aspect)) & ~1;
  SetIfDefault("video_mode_width", "1282");
  SetIfDefault("video_mode_height", std::to_string(kGuestHeight));
  SetIfDefault("window_width", std::to_string(window_width));
  SetIfDefault("window_height", std::to_string(window_height));
  SetIfDefault("present_letterbox", "false");
  REXLOG_INFO("{}: aspect {:.4f}, window {}x{}", wide ? "Ultrawide" : "Portrait", aspect,
              window_width, window_height);
}

float UltrawideAspect() {
  return g_camera_aspect.load(std::memory_order_relaxed);
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

namespace {

// With another aspect the game must still see exactly 1280x720: it copies the
// video mode into its display parameters, and any other size there loses
// glyphs from its UI text.
void ReportGuestVideoMode(uint8_t* base, uint32_t mode) {
  if (nhl3on3::UltrawideAspect() <= 0.0f || !mode) {
    return;
  }
  uint8_t* p = base + mode;
  for (auto [offset, value] : {std::pair{0u, 1280u}, std::pair{4u, 720u}}) {
    p[offset] = uint8_t(value >> 24);
    p[offset + 1] = uint8_t(value >> 16);
    p[offset + 2] = uint8_t(value >> 8);
    p[offset + 3] = uint8_t(value);
  }
}

}  // namespace

// (X_VIDEO_MODE* out): display width at +0, height at +4.
extern "C" REX_FUNC(__imp__XGetVideoMode) {
  static const nhl3on3::GuestFunc original =
      nhl3on3::ResolveRuntimeExport("__imp__XGetVideoMode");
  const uint32_t mode = ctx.r3.u32;
  original(ctx, base);
  ReportGuestVideoMode(base, mode);
}

extern "C" REX_FUNC(__imp__VdQueryVideoMode) {
  static const nhl3on3::GuestFunc original =
      nhl3on3::ResolveRuntimeExport("__imp__VdQueryVideoMode");
  const uint32_t mode = ctx.r3.u32;
  original(ctx, base);
  ReportGuestVideoMode(base, mode);
}

// (swap packet address in the ring, ...)
extern "C" REX_FUNC(__imp__VdSwap) {
  static const nhl3on3::GuestFunc original = nhl3on3::ResolveRuntimeExport("__imp__VdSwap");
  static std::once_flag draw_hooks;
  std::call_once(draw_hooks, nhl3on3::InstallDrawHooks);
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

namespace topdown {

struct Vec3 {
  float v[3];
  float& operator[](int i) { return v[i]; }
  float operator[](int i) const { return v[i]; }
};

Vec3 operator+(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 operator*(const Vec3& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
float Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 Axis(int i, float sign) {
  Vec3 out{};
  out[i] = sign;
  return out;
}

float LoadFloat(const uint8_t* p) {
  const uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

void StoreFloat(uint8_t* p, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  p[0] = uint8_t(bits >> 24);
  p[1] = uint8_t(bits >> 16);
  p[2] = uint8_t(bits >> 8);
  p[3] = uint8_t(bits);
}

// The camera matrix rows are right, up, at (view direction), position.
Vec3 LoadRow(const uint8_t* matrix, int row) {
  const uint8_t* p = matrix + row * 16;
  return {LoadFloat(p), LoadFloat(p + 4), LoadFloat(p + 8)};
}

void StoreRow(uint8_t* matrix, int row, const Vec3& value) {
  uint8_t* p = matrix + row * 16;
  for (int i = 0; i < 3; ++i) {
    StoreFloat(p + i * 4, value[i]);
  }
}

std::string Format(const Vec3& v) { return fmt::format("({:.2f}, {:.2f}, {:.2f})", v[0], v[1], v[2]); }

// World axes and distance come from the stock camera when a match starts: its
// up row picks the world up axis, its view direction picks the rink's long axis
// and its distance to the ice sets the framing. The ice is assumed to be the
// plane through the origin with center ice at the origin.
//
// For the first seconds of a match the camera is an identity placeholder. The
// real one watches from the side of the rink, and the top of the screen is
// the end opposite its screen right; if the side camera moves to the other
// side of the rink, the top of the screen follows.
struct State {
  bool latched = false;
  std::chrono::steady_clock::time_point last_call;
  Vec3 world_up{};
  Vec3 rink_long{};
  Vec3 written_at{};
  float distance = 0.0f;
  float handedness = 1.0f;
  int logged = 0;
};

bool Near(const Vec3& a, const Vec3& b) {
  return std::fabs(a[0] - b[0]) < 1e-3f && std::fabs(a[1] - b[1]) < 1e-3f &&
         std::fabs(a[2] - b[2]) < 1e-3f;
}

bool IsPlaceholder(const Vec3& right, const Vec3& up, const Vec3& at) {
  return Near(right, Axis(0, 1.0f)) && Near(up, Axis(1, 1.0f)) && Near(at, Axis(2, 1.0f));
}

// Points rink_long at the end opposite the side camera's screen right. Shots
// looking down the rink (replays, celebrations) don't show which side the
// camera is on, so only side views count.
void FollowStockDirection(State& state, const Vec3& right, const Vec3& up, const Vec3& at) {
  if (IsPlaceholder(right, up, at) || Near(at, state.written_at)) {
    return;
  }
  const float along = Dot(right, state.rink_long);
  if (along < 0.5f) {
    return;
  }
  state.rink_long = state.rink_long * -1.0f;
  REXLOG_INFO("Topdown: side camera right {}; screen up is {}", Format(right),
              Format(state.rink_long));
}

// The game turns the sticks into directions on the ice with its own camera,
// not the top-down one: stick right is that camera's right, stick up its
// forward. The map takes a stick as meant on the top-down screen to the stick
// that gives the same direction through the game's camera.
struct StickMap {
  float m[4] = {1.0f, 0.0f, 0.0f, 1.0f};  // x' = m0 x + m1 y, y' = m2 x + m3 y
  std::chrono::steady_clock::time_point updated;
};
std::mutex g_stick_mutex;
StickMap g_stick;

Vec3 OnIce(const Vec3& v, const Vec3& world_up) {
  const Vec3 flat = v + world_up * -Dot(v, world_up);
  const float length = std::sqrt(Dot(flat, flat));
  return length > 1e-4f ? flat * (1.0f / length) : Vec3{};
}

void UpdateStickMap(const Vec3& world_up, const Vec3& right, const Vec3& up, const Vec3& at,
                    const Vec3& view_right, const Vec3& view_up) {
  const Vec3 stock_right = OnIce(right, world_up);
  // Forward on the ice: across stock_right, on the side the camera faces.
  Vec3 stock_forward = OnIce(Cross(world_up, stock_right), world_up);
  if (Dot(stock_forward, OnIce(at, world_up) + OnIce(up, world_up)) < 0.0f) {
    stock_forward = stock_forward * -1.0f;
  }
  const Vec3 target_right = OnIce(view_right, world_up);
  const Vec3 target_up = OnIce(view_up, world_up);
  const float m[4] = {Dot(target_right, stock_right), Dot(target_up, stock_right),
                      Dot(target_right, stock_forward), Dot(target_up, stock_forward)};
  static float logged[4] = {};
  bool changed = false;
  for (int i = 0; i < 4; ++i) {
    changed |= std::fabs(m[i] - logged[i]) > 0.25f;
  }
  if (changed) {
    std::memcpy(logged, m, sizeof(m));
    REXLOG_INFO("Topdown: sticks x' = {:.2f} x + {:.2f} y, y' = {:.2f} x + {:.2f} y "
                "(game camera right {} forward {})",
                m[0], m[1], m[2], m[3], Format(stock_right), Format(stock_forward));
  }
  std::lock_guard lock(g_stick_mutex);
  std::memcpy(g_stick.m, m, sizeof(m));
  g_stick.updated = std::chrono::steady_clock::now();
}

void Apply(uint8_t* matrix) {
  using Clock = std::chrono::steady_clock;
  static State state;
  const Clock::time_point now = Clock::now();
  if (now - state.last_call > std::chrono::seconds(1)) {
    state.latched = false;
    state.logged = 0;
  }
  state.last_call = now;

  const Vec3 right = LoadRow(matrix, 0);
  const Vec3 up = LoadRow(matrix, 1);
  const Vec3 at = LoadRow(matrix, 2);
  const Vec3 pos = LoadRow(matrix, 3);
  if (state.logged < 3) {
    static Clock::time_point next_log;
    if (now >= next_log) {
      next_log = now + std::chrono::seconds(2);
      ++state.logged;
      REXLOG_INFO("Topdown: stock camera right {} up {} at {} pos {}", Format(right), Format(up),
                  Format(at), Format(pos));
    }
  }

  if (!state.latched) {
    int up_axis = 0;
    for (int i = 1; i < 3; ++i) {
      if (std::fabs(up[i]) > std::fabs(up[up_axis])) {
        up_axis = i;
      }
    }
    state.world_up = Axis(up_axis, up[up_axis] < 0.0f ? -1.0f : 1.0f);
    int long_axis = up_axis == 0 ? 1 : 0;
    for (int i = 0; i < 3; ++i) {
      if (i != up_axis && std::fabs(at[i]) > std::fabs(at[long_axis])) {
        long_axis = i;
      }
    }
    // Opposite the placeholder's look direction: that placeholder faces the end
    // the centre-ice logo reads as upside down.
    state.rink_long = Axis(long_axis, at[long_axis] < 0.0f ? 1.0f : -1.0f);
    const float height = Dot(pos, state.world_up);
    const float descent = -Dot(at, state.world_up);
    const float to_ice = descent > 0.05f ? height / descent : 0.0f;
    const float from_origin = std::sqrt(Dot(pos, pos));
    state.distance = to_ice > 0.0f ? to_ice : from_origin;
    state.handedness = Dot(right, Cross(up, at)) < 0.0f ? -1.0f : 1.0f;
    state.latched = true;
    REXLOG_INFO(
        "Topdown: world up {} rink long axis {} camera height {:.2f}, distance to ice {:.2f}, "
        "to origin {:.2f}, looking at {}, handedness {}",
        Format(state.world_up), Format(state.rink_long), height, to_ice, from_origin,
        Format(pos + at * to_ice), state.handedness);
  }
  FollowStockDirection(state, right, up, at);

  const Vec3 screen_up =
      REXCVAR_GET(topdown_direction) == "reverse" ? state.rink_long * -1.0f : state.rink_long;
  const float tilt = float(std::clamp(REXCVAR_GET(topdown_tilt), 0.0, 80.0) * 3.14159265358979 / 180.0);
  const float zoom = float(std::clamp(REXCVAR_GET(topdown_zoom), 0.1, 10.0));
  const Vec3 new_at = state.world_up * -std::cos(tilt) + screen_up * std::sin(tilt);
  const Vec3 new_up = state.world_up * std::sin(tilt) + screen_up * std::cos(tilt);
  const Vec3 new_right = Cross(new_up, new_at) * state.handedness;
  const Vec3 new_pos = new_at * -(state.distance * zoom);
  if (!IsPlaceholder(right, up, at) && !Near(at, state.written_at)) {
    UpdateStickMap(state.world_up, right, up, at, new_right, screen_up);
  } else if (Near(at, state.written_at)) {
    // The game's camera didn't move since the last frame: same map, still live.
    std::lock_guard lock(g_stick_mutex);
    g_stick.updated = now;
  }
  StoreRow(matrix, 0, new_right);
  StoreRow(matrix, 1, new_up);
  StoreRow(matrix, 2, new_at);
  StoreRow(matrix, 3, new_pos);
  state.written_at = new_at;
}

}  // namespace topdown

namespace nhl3on3 {

void RemapPadSticks(uint8_t* base, uint32_t state) {
  if (!state) {
    return;
  }
  float m[4];
  {
    std::lock_guard lock(topdown::g_stick_mutex);
    // Only while the top-down camera is running (menus keep the stock sticks).
    if (std::chrono::steady_clock::now() - topdown::g_stick.updated >
        std::chrono::milliseconds(500)) {
      return;
    }
    std::memcpy(m, topdown::g_stick.m, sizeof(m));
  }
  // X_INPUT_STATE: left stick x, y at +8, +10; right stick at +12, +14.
  for (uint32_t offset : {8u, 12u}) {
    uint8_t* p = base + state + offset;
    const float x = float(int16_t(uint16_t(p[0] << 8 | p[1])));
    const float y = float(int16_t(uint16_t(p[2] << 8 | p[3])));
    auto pack = [](float v) {
      return uint16_t(int16_t(std::clamp(std::lround(v), -32768l, 32767l)));
    };
    const uint16_t nx = pack(m[0] * x + m[1] * y);
    const uint16_t ny = pack(m[2] * x + m[3] * y);
    p[0] = uint8_t(nx >> 8);
    p[1] = uint8_t(nx);
    p[2] = uint8_t(ny >> 8);
    p[3] = uint8_t(ny);
  }
}

}  // namespace nhl3on3

// r25 = camera (matrix at +0), f1 = render target aspect, f13 / f0 = view
// window width / height about to be stored.
void Nhl3on3GameCameraWindowHook(PPCRegister& r25, PPCRegister& f0, PPCRegister& f1,
                                 PPCRegister& f13) {
  const float aspect = g_camera_aspect.load(std::memory_order_relaxed);
  if (aspect <= 0.0f || std::fabs(float(f1.f64) - float(kNativeAspect)) >= 0.001f) {
    return;
  }
  const bool topdown =
      aspect < float(kNativeAspect) && REXCVAR_GET(portrait_camera) == "topdown";
  if (topdown) {
    if (auto* memory = REX_KERNEL_MEMORY()) {
      topdown::Apply(memory->virtual_membase() + r25.u32);
    }
  }
  const float ratio = float(aspect / kNativeAspect);
  const bool hor_plus = ratio > 1.0f && REXCVAR_GET(ultrawide_fov) != "vert-";
  static thread_local float last_ratio = 0.0f;
  static thread_local bool last_topdown = false;
  if (ratio != last_ratio || topdown != last_topdown) {
    last_ratio = ratio;
    last_topdown = topdown;
    const double width = hor_plus ? f13.f64 * ratio : f13.f64;
    const double height = hor_plus ? f0.f64 : f0.f64 / ratio;
    REXLOG_INFO("Ultrawide: game camera view window {:.4f} x {:.4f} -> {:.4f} x {:.4f} ({})",
                f13.f64, f0.f64, width, height,
                topdown ? "topdown" : (hor_plus ? "hor+" : (ratio > 1.0f ? "vert-" : "vert+")));
  }
  if (hor_plus) {
    f13.f64 = double(float(f13.f64) * ratio);
  } else {
    f0.f64 = double(float(f0.f64) / ratio);
  }
}

// r31 = frustum: horizontal and vertical half-angles (radians, big-endian
// floats) at +0 / +4.
void Nhl3on3CullFrustumHook(PPCRegister& r31) {
  const float aspect = g_camera_aspect.load(std::memory_order_relaxed);
  if (aspect <= 0.0f) {
    return;
  }
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) {
    return;
  }
  const float ratio = float(aspect / kNativeAspect);
  const bool hor_plus = ratio > 1.0f && REXCVAR_GET(ultrawide_fov) != "vert-";
  uint8_t* angle = memory->virtual_membase() + r31.u32 + (hor_plus ? 0 : 4);
  const float widen = hor_plus ? ratio : 1.0f / ratio;
  if (widen <= 1.0f) {
    return;
  }
  const float half = topdown::LoadFloat(angle);
  if (!(half > 0.0f && half < 1.5f)) {
    return;
  }
  topdown::StoreFloat(angle, std::min(std::atan(std::tan(half) * widen), 1.5f));
}

// r31 = projected point: x, y in 1280x720 frame pixels (big-endian floats).
void Nhl3on3ScreenProjectHook(PPCRegister& r31) {
  float kx, ky, cy;
  nhl3on3::UiSqueeze(kx, ky, cy);
  if (kx >= 1.0f && ky >= 1.0f) {
    return;
  }
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) {
    return;
  }
  uint8_t* point = memory->virtual_membase() + r31.u32;
  const float x = topdown::LoadFloat(point);
  const float y = topdown::LoadFloat(point + 4);
  topdown::StoreFloat(point, 640.0f + (x - 640.0f) / kx);
  topdown::StoreFloat(point + 4, cy + (y - cy) / ky);
}
