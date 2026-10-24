// Ultrawide: per-draw adjustments in the GPU command processor.
//
// The game always renders a 1280x720 frame, which the presenter stretches to
// the display's aspect ratio. The 3D view is widened to match in the camera
// code (nhl3on3_display.cpp); 2D draws are squeezed into the centre here, so
// the stretch brings them back to 16:9 proportions. Only draws into the
// 1280-wide frame with a known 2D pixel shader count as 2D; the 3D scene is
// drawn with other shaders and stays full width.

#include "nhl3on3_display.h"
#include "nhl3on3_vr.h"
#include "nhl3on3_vtable.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/register_file.h>
#include <rex/logging.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(draw_trace, false, "NHL3on3/Debug",
                    "Log a summary of each frame's draws (shaders, viewport, render target) "
                    "every 5 seconds");

REXCVAR_DEFINE_BOOL(ultrawide_ui_16x9, true, "NHL3on3/Display",
                    "With an ultrawide or portrait aspect_ratio, keep menus, HUD and movies at "
                    "16:9 in the centre of the screen; only the 3D game view fills it");

REXCVAR_DEFINE_STRING(portrait_ui_position, "top", "NHL3on3/Display",
                      "With a portrait aspect_ratio and ultrawide_ui_16x9, where the 16:9 "
                      "menus and HUD sit: top, center or bottom");

REXCVAR_DEFINE_STRING(ultrawide_ui_shaders,
                      "F07567B2B29BD61D,E3E21FD906F68A1C,5773DC18083C4C20",
                      "NHL3on3/Debug",
                      "Pixel shader hashes (as draw_trace logs them, separated by ',') whose "
                      "full-frame draws are 2D and kept at 16:9 by ultrawide_ui_16x9");

namespace {

namespace gfx = rex::graphics;

struct Access : gfx::CommandProcessor {
  using DrawFn = bool (gfx::CommandProcessor::*)(gfx::xenos::PrimitiveType, uint32_t,
                                                 IndexBufferInfo*, bool);
  using SwapFn = void (gfx::CommandProcessor::*)(uint32_t, uint32_t, uint32_t);
  static DrawFn Draw() { return &Access::IssueDraw; }
  static SwapFn Swap() { return &Access::IssueSwap; }
};

using DrawThunk = bool (*)(gfx::CommandProcessor*, gfx::xenos::PrimitiveType, uint32_t, void*,
                           bool);
using SwapThunk = void (*)(gfx::CommandProcessor*, uint32_t, uint32_t, uint32_t);

DrawThunk g_draw = nullptr;
SwapThunk g_swap = nullptr;
gfx::RegisterFile* g_regs = nullptr;

using nhl3on3::PatchSlot;
using nhl3on3::VtableOffset;

// --- draw_trace -------------------------------------------------------------

struct DrawKey {
  uint64_t vs, ps;
  uint32_t vte, xscale, xoffset, yscale, yoffset, surface, color, scissor_tl, scissor_br;
  uint32_t tex_w, tex_h;
  bool ui;
  auto Tie() const {
    return std::tie(vs, ps, vte, xscale, xoffset, yscale, yoffset, surface, color, scissor_tl,
                    scissor_br, tex_w, tex_h, ui);
  }
  bool operator<(const DrawKey& o) const { return Tie() < o.Tie(); }
};

struct DrawStats {
  uint32_t count = 0;
  uint32_t first_index = 0;
};

std::mutex g_trace_mutex;
std::map<DrawKey, DrawStats> g_frame;
uint32_t g_frame_draws = 0;
uint64_t g_last_dump = 0;
uint64_t g_frames = 0;

float AsFloat(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// Largest 2D texture the active pixel shader samples (0 x 0 if none, or if
// the shader hasn't been analysed yet).
void LargestTexture(gfx::CommandProcessor* cp, uint32_t& width, uint32_t& height) {
  width = height = 0;
  auto* ps = cp->active_pixel_shader();
  if (!ps) {
    return;
  }
  for (const auto& binding : ps->texture_bindings()) {
    const auto fetch = g_regs->GetTextureFetch(binding.fetch_constant);
    if (fetch.dimension != gfx::xenos::DataDimension::k2DOrStacked) {
      continue;
    }
    const uint32_t w = fetch.size_2d.width + 1;
    const uint32_t h = fetch.size_2d.height + 1;
    if (w * h > width * height) {
      width = w;
      height = h;
    }
  }
}

void TraceDraw(gfx::CommandProcessor* cp, bool ui) {
  const auto& r = *g_regs;
  DrawKey key{};
  key.ui = ui;
  auto* vs = cp->active_vertex_shader();
  auto* ps = cp->active_pixel_shader();
  key.vs = vs ? vs->ucode_data_hash() : 0;
  key.ps = ps ? ps->ucode_data_hash() : 0;
  key.vte = r[gfx::XE_GPU_REG_PA_CL_VTE_CNTL];
  key.xscale = r[gfx::XE_GPU_REG_PA_CL_VPORT_XSCALE];
  key.xoffset = r[gfx::XE_GPU_REG_PA_CL_VPORT_XOFFSET];
  key.yscale = r[gfx::XE_GPU_REG_PA_CL_VPORT_YSCALE];
  key.yoffset = r[gfx::XE_GPU_REG_PA_CL_VPORT_YOFFSET];
  key.surface = r[gfx::XE_GPU_REG_RB_SURFACE_INFO];
  key.color = r[gfx::XE_GPU_REG_RB_COLOR_INFO];
  key.scissor_tl = r[gfx::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  key.scissor_br = r[gfx::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  LargestTexture(cp, key.tex_w, key.tex_h);
  std::lock_guard lock(g_trace_mutex);
  auto& s = g_frame[key];
  if (!s.count++) {
    s.first_index = g_frame_draws;
  }
  ++g_frame_draws;
}

void DumpFrame() {
  std::lock_guard lock(g_trace_mutex);
  ++g_frames;
  if (g_frames - g_last_dump >= 300) {
    g_last_dump = g_frames;
    std::vector<std::pair<DrawKey, DrawStats>> rows(g_frame.begin(), g_frame.end());
    std::sort(rows.begin(), rows.end(),
              [](auto& a, auto& b) { return a.second.first_index < b.second.first_index; });
    REXLOG_INFO("[draws] frame {}: {} draws in {} groups", g_frames, g_frame_draws, rows.size());
    for (auto& [k, s] : rows) {
      REXLOG_INFO(
          "[draws]  #{:4} x{:4} vs {:016X} ps {:016X} vte {:08X} vp x {:.1f}+{:.1f} y {:.1f}+{:.1f} "
          "surf {:08X} color {:08X} scissor {:08X}-{:08X} tex {}x{}{}",
          s.first_index, s.count, k.vs, k.ps, k.vte, AsFloat(k.xscale), AsFloat(k.xoffset),
          AsFloat(k.yscale), AsFloat(k.yoffset), k.surface, k.color, k.scissor_tl, k.scissor_br,
          k.tex_w, k.tex_h, k.ui ? " ui16:9" : "");
    }
  }
  g_frame.clear();
  g_frame_draws = 0;
}

// --- 16:9 UI ------------------------------------------------------------------
//
// Menus, HUD and movies are drawn straight into the 1280-wide frame with a few
// 2D pixel shaders. Their viewport (and scissor) is narrowed around the centre
// by 16:9 / aspect (ultrawide), or flattened by aspect / 16:9 towards the top,
// centre or bottom (portrait), so the presenter's stretch restores their
// proportions.

constexpr uint32_t kFrameWidth = 1280;
constexpr uint32_t kFrameHeight = 720;

std::string g_ui_list;
std::set<uint64_t> g_ui_shaders;
// Read by the game thread too (nhl3on3::UiSqueeze).
std::atomic<float> g_ui_squeeze_x{1.0f};  // 1 = off
std::atomic<float> g_ui_squeeze_y{1.0f};
std::atomic<float> g_ui_centre_y{kFrameHeight / 2.0f};  // frame row ky scales about

void RefreshUiSettings() {
  const float aspect = nhl3on3::UltrawideAspect();
  const float native = float(16.0 / 9.0);
  const bool on = REXCVAR_GET(ultrawide_ui_16x9) && aspect > 0.0f;
  g_ui_squeeze_x.store((on && aspect > native) ? native / aspect : 1.0f,
                       std::memory_order_relaxed);
  g_ui_squeeze_y.store((on && aspect < native) ? aspect / native : 1.0f,
                       std::memory_order_relaxed);
  const std::string position = REXCVAR_GET(portrait_ui_position);
  g_ui_centre_y.store(position == "center" ? kFrameHeight / 2.0f
                      : position == "bottom" ? float(kFrameHeight)
                                             : 0.0f,
                      std::memory_order_relaxed);
  if (nhl3on3::vr::Enabled()) {
    float kx, ky;
    nhl3on3::vr::HudSqueeze(kx, ky);
    g_ui_squeeze_x.store(kx, std::memory_order_relaxed);
    g_ui_squeeze_y.store(ky, std::memory_order_relaxed);
    g_ui_centre_y.store(kFrameHeight / 2.0f, std::memory_order_relaxed);
  }
  const std::string list = REXCVAR_GET(ultrawide_ui_shaders);
  if (list == g_ui_list) {
    return;
  }
  g_ui_list = list;
  g_ui_shaders.clear();
  size_t pos = 0;
  while (pos < list.size()) {
    size_t end = list.find(',', pos);
    if (end == std::string::npos) {
      end = list.size();
    }
    std::string_view item(list.data() + pos, end - pos);
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
    uint64_t hash = 0;
    auto [ptr, ec] = std::from_chars(item.data(), item.data() + item.size(), hash, 16);
    if (ec == std::errc() && ptr == item.data() + item.size()) {
      g_ui_shaders.insert(hash);
    } else if (!item.empty()) {
      REXLOG_WARN("ultrawide_ui_shaders: '{}' isn't a shader hash", item);
    }
    pos = end + 1;
  }
}

// Set once the 3D scene (drawn multisampled) has been rendered this frame.
bool g_scene_drawn = false;

bool IsUiDraw(gfx::CommandProcessor* cp) {
  if (g_ui_squeeze_x.load(std::memory_order_relaxed) >= 1.0f &&
      g_ui_squeeze_y.load(std::memory_order_relaxed) >= 1.0f) {
    return false;
  }
  const uint32_t surface = (*g_regs)[gfx::XE_GPU_REG_RB_SURFACE_INFO];
  if ((surface >> 16) & 3) {
    g_scene_drawn = true;
  }
  if ((surface & 0x3FFF) != kFrameWidth) {
    return false;
  }
  auto* ps = cp->active_pixel_shader();
  if (!ps || !g_ui_shaders.count(ps->ucode_data_hash())) {
    return false;
  }
  // The finished 3D scene (with the in-game HUD drawn into it) is copied to
  // the frame with the same 2D shaders; that copy has to stay full screen.
  // Movies also sample frame-sized textures but come without a 3D pass.
  if (g_scene_drawn) {
    uint32_t w, h;
    LargestTexture(cp, w, h);
    if (w >= kFrameWidth && h >= kFrameHeight) {
      return false;
    }
  }
  return true;
}

uint32_t SqueezeCoord(uint32_t v, float k, float c, uint32_t size) {
  return uint32_t(std::clamp(std::lround(c + (float(v) - c) * k), 0l, long(size)));
}

struct SavedRegs {
  uint32_t vte, xscale, xoffset, yscale, yoffset, scissor_tl, scissor_br;
};

// Scales one viewport axis by k around frame coordinate c. Screen = scale * v +
// offset; with scale/offset disabled in VTE_CNTL they're 1 and 0.
void SqueezeAxis(uint32_t vte, uint32_t scale_bit, uint32_t scale_reg, uint32_t offset_reg,
                 float k, float c) {
  auto& r = *g_regs;
  const float scale = (vte & scale_bit) ? AsFloat(r[scale_reg]) : 1.0f;
  const float offset = (vte & (scale_bit << 1)) ? AsFloat(r[offset_reg]) : 0.0f;
  const float new_scale = scale * k;
  const float new_offset = c + (offset - c) * k;
  std::memcpy(&r[scale_reg], &new_scale, 4);
  std::memcpy(&r[offset_reg], &new_offset, 4);
}

// Scissor TL / BR hold x in bits 0-13 and y in bits 16-29.
void SqueezeScissor(uint32_t shift, float k, float c, uint32_t size) {
  auto& r = *g_regs;
  const uint32_t mask = 0x3FFFu << shift;
  uint32_t& tl = r[gfx::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
  uint32_t& br = r[gfx::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
  const uint32_t lo = (tl & mask) >> shift;
  const uint32_t hi = (br & mask) >> shift;
  if (lo > 0 || hi < size) {
    tl = (tl & ~mask) | (SqueezeCoord(lo, k, c, size) << shift);
    br = (br & ~mask) | (SqueezeCoord(std::min(hi, size), k, c, size) << shift);
  }
}

SavedRegs Squeeze(float kx, float ky, float cy) {
  auto& r = *g_regs;
  SavedRegs saved{r[gfx::XE_GPU_REG_PA_CL_VTE_CNTL],      r[gfx::XE_GPU_REG_PA_CL_VPORT_XSCALE],
                  r[gfx::XE_GPU_REG_PA_CL_VPORT_XOFFSET], r[gfx::XE_GPU_REG_PA_CL_VPORT_YSCALE],
                  r[gfx::XE_GPU_REG_PA_CL_VPORT_YOFFSET], r[gfx::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
                  r[gfx::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR]};
  uint32_t vte = saved.vte;
  if (kx < 1.0f) {
    SqueezeAxis(saved.vte, 1, gfx::XE_GPU_REG_PA_CL_VPORT_XSCALE,
                gfx::XE_GPU_REG_PA_CL_VPORT_XOFFSET, kx, kFrameWidth / 2.0f);
    SqueezeScissor(0, kx, kFrameWidth / 2.0f, kFrameWidth);
    vte |= 3;
  }
  if (ky < 1.0f) {
    SqueezeAxis(saved.vte, 4, gfx::XE_GPU_REG_PA_CL_VPORT_YSCALE,
                gfx::XE_GPU_REG_PA_CL_VPORT_YOFFSET, ky, cy);
    SqueezeScissor(16, ky, cy, kFrameHeight);
    vte |= 0xC;
  }
  r[gfx::XE_GPU_REG_PA_CL_VTE_CNTL] = vte;
  return saved;
}

void Restore(const SavedRegs& saved) {
  auto& r = *g_regs;
  r[gfx::XE_GPU_REG_PA_CL_VTE_CNTL] = saved.vte;
  r[gfx::XE_GPU_REG_PA_CL_VPORT_XSCALE] = saved.xscale;
  r[gfx::XE_GPU_REG_PA_CL_VPORT_XOFFSET] = saved.xoffset;
  r[gfx::XE_GPU_REG_PA_CL_VPORT_YSCALE] = saved.yscale;
  r[gfx::XE_GPU_REG_PA_CL_VPORT_YOFFSET] = saved.yoffset;
  r[gfx::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL] = saved.scissor_tl;
  r[gfx::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = saved.scissor_br;
}

// Top-down roof cut, refreshed every frame.
bool g_roof_active = false;
float g_roof_plane[4] = {};

// 3D scene draws go to the multisampled frame-wide surface; 2D shaders drawn
// there (the in-game HUD) are in screen space and must not be cut.
bool IsSceneDraw(gfx::CommandProcessor* cp) {
  const uint32_t surface = (*g_regs)[gfx::XE_GPU_REG_RB_SURFACE_INFO];
  if (!((surface >> 16) & 3) || (surface & 0x3FFF) != kFrameWidth) {
    return false;
  }
  auto* ps = cp->active_pixel_shader();
  return !ps || !g_ui_shaders.count(ps->ucode_data_hash());
}

bool DrawWithRoofCut(gfx::CommandProcessor* cp, gfx::xenos::PrimitiveType prim, uint32_t count,
                     void* index_buffer, bool major_mode_explicit) {
  auto& r = *g_regs;
  uint32_t saved[5];
  saved[0] = r[gfx::XE_GPU_REG_PA_CL_CLIP_CNTL];
  for (int i = 0; i < 4; ++i) {
    saved[1 + i] = r[gfx::XE_GPU_REG_PA_CL_UCP_0_X + i];
    std::memcpy(&r[gfx::XE_GPU_REG_PA_CL_UCP_0_X + i], &g_roof_plane[i], 4);
  }
  r[gfx::XE_GPU_REG_PA_CL_CLIP_CNTL] = saved[0] | 1;
  const bool result = g_draw(cp, prim, count, index_buffer, major_mode_explicit);
  r[gfx::XE_GPU_REG_PA_CL_CLIP_CNTL] = saved[0];
  for (int i = 0; i < 4; ++i) {
    r[gfx::XE_GPU_REG_PA_CL_UCP_0_X + i] = saved[1 + i];
  }
  return result;
}

bool HookedDraw(gfx::CommandProcessor* cp, gfx::xenos::PrimitiveType prim, uint32_t count,
                void* index_buffer, bool major_mode_explicit) {
  const bool ui = IsUiDraw(cp);
  if (REXCVAR_GET(draw_trace)) {
    TraceDraw(cp, ui);
  }
  if (!ui) {
    if (g_roof_active && IsSceneDraw(cp)) {
      return DrawWithRoofCut(cp, prim, count, index_buffer, major_mode_explicit);
    }
    return g_draw(cp, prim, count, index_buffer, major_mode_explicit);
  }
  const SavedRegs saved = Squeeze(g_ui_squeeze_x.load(std::memory_order_relaxed),
                                  g_ui_squeeze_y.load(std::memory_order_relaxed),
                                  g_ui_centre_y.load(std::memory_order_relaxed));
  const bool result = g_draw(cp, prim, count, index_buffer, major_mode_explicit);
  Restore(saved);
  return result;
}

void HookedSwap(gfx::CommandProcessor* cp, uint32_t ptr, uint32_t width, uint32_t height) {
  if (REXCVAR_GET(draw_trace)) {
    DumpFrame();
  }
  RefreshUiSettings();
  g_scene_drawn = false;
  g_roof_active = nhl3on3::RoofClipPlane(g_roof_plane);
  g_swap(cp, ptr, width, height);
  if (nhl3on3::vr::Enabled()) {
    nhl3on3::vr::OnHostSwap();
  }
}

}  // namespace

namespace nhl3on3 {

void InstallDrawHooks() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  auto* runtime = rex::Runtime::instance();
  auto* system = runtime ? static_cast<gfx::GraphicsSystem*>(runtime->graphics_system()) : nullptr;
  auto* cp = system ? system->command_processor() : nullptr;
  if (!cp) {
    REXLOG_WARN("Ultrawide: no GPU command processor; draw hooks off");
    return;
  }
  g_regs = system->register_file();
  const ptrdiff_t draw_offset = VtableOffset(Access::Draw());
  const ptrdiff_t swap_offset = VtableOffset(Access::Swap());
  if (draw_offset < 0 || swap_offset < 0) {
    REXLOG_WARN("Ultrawide: couldn't locate IssueDraw/IssueSwap in the vtable; draw hooks off");
    return;
  }
  void** vtable = *reinterpret_cast<void***>(cp);
  const bool ok =
      PatchSlot(vtable + draw_offset / ptrdiff_t(sizeof(void*)), &HookedDraw, g_draw) &&
      PatchSlot(vtable + swap_offset / ptrdiff_t(sizeof(void*)), &HookedSwap, g_swap);
  RefreshUiSettings();
  if (ok && nhl3on3::vr::Enabled()) {
    nhl3on3::vr::InstallPresenterHook();
  }
  REXLOG_INFO(
      "Ultrawide: draw hooks {} (IssueDraw slot {}, IssueSwap slot {}), 2D squeezed to {:.3f} x "
      "{:.3f}",
      ok ? "installed" : "FAILED", draw_offset / 8, swap_offset / 8, g_ui_squeeze_x.load(),
      g_ui_squeeze_y.load());
}

void UiSqueeze(float& x, float& y, float& centre_y) {
  x = g_ui_squeeze_x.load(std::memory_order_relaxed);
  y = g_ui_squeeze_y.load(std::memory_order_relaxed);
  centre_y = g_ui_centre_y.load(std::memory_order_relaxed);
}

}  // namespace nhl3on3
