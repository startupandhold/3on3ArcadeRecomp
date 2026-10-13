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
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

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

extern "C" REX_FUNC(__imp__VdSwap) {
  static const nhl3on3::GuestFunc original = nhl3on3::ResolveRuntimeExport("__imp__VdSwap");
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
