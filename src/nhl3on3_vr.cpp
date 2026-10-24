// VR: OpenXR output with alternate-eye stereo.
//
// The game renders one frame at a time, so each frame is drawn for one eye:
// the match camera takes that eye's tracked pose (nhl3on3_display.cpp), the
// next frame the other eye's. Three threads are involved:
//  - game thread: picks the eye at each VdSwap and queues it for the GPU
//    thread, with the pose the frame is drawn from;
//  - GPU thread: when the command processor finishes a swap, copies the
//    presenter's guest output image into that eye's texture (or the menu
//    screen's, for frames without the match camera);
//  - XR thread: runs the OpenXR frame loop and draws the latest textures into
//    the headset swapchains, each with the pose it was drawn from, so the
//    runtime can reproject it to the current head pose.
// All GPU work goes to the provider's direct queue, which orders the copies
// after the frame they read and before the draws that read them.

#include "nhl3on3_vr.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(vr, false, "NHL3on3/VR",
                    "Play in a VR headset through OpenXR (Windows): matches in 3D on a "
                    "tabletop rink, menus on a virtual screen. Needs an OpenXR runtime "
                    "(Virtual Desktop's VDXR, SteamVR or Oculus) and about 120 FPS: each eye "
                    "gets every other frame")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_DOUBLE(vr_scale, 0.01, "NHL3on3/VR",
                      "Size of the rink: metres per game unit (at 0.01 the rink is roughly "
                      "a metre long)");

REXCVAR_DEFINE_DOUBLE(vr_table_distance, 0.75, "NHL3on3/VR",
                      "How far in front of you the centre of the rink is, in metres");

REXCVAR_DEFINE_DOUBLE(vr_table_height, 0.5, "NHL3on3/VR",
                      "How far below your eyes the ice is, in metres");

REXCVAR_DEFINE_DOUBLE(vr_screen_distance, 2.0, "NHL3on3/VR",
                      "Distance to the virtual screen for menus, in metres");

REXCVAR_DEFINE_DOUBLE(vr_screen_width, 2.4, "NHL3on3/VR",
                      "Width of the virtual screen for menus, in metres");

REXCVAR_DEFINE_DOUBLE(vr_hud_scale, 0.5, "NHL3on3/VR",
                      "Width of the in-match HUD as a fraction of the view");

REXCVAR_DEFINE_INT32(vr_frame_lag, 1, "NHL3on3/VR",
                     "How many frames after its camera is set the game shows a frame (0-2). "
                     "Wrong, the rink swims as you move your head; F9 cycles it while playing");

#if !defined(_WIN32)

namespace nhl3on3::vr {

bool Enabled() { return false; }
void TableTransform(float& scale, float centre[3]) {
  scale = 0.01f;
  centre[0] = centre[1] = centre[2] = 0.0f;
}
bool FrameEye(EyeView&) { return false; }
void NoteStereoFrame() {}
void OnGuestSwap() {}
void InstallPresenterHook() {}
void OnHostSwap() {}
void HudSqueeze(float& x, float& y) { x = y = 1.0f; }

}  // namespace nhl3on3::vr

#else

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/graphics/graphics_system.h>
#include <rex/runtime.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/presenter.h>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "nhl3on3_vtable.h"

namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using nhl3on3::vr::EyeView;
using GuestOutputContext = rex::ui::Presenter::GuestOutputRefreshContext;

// How long after the last stereo frame the headset keeps showing the rink.
constexpr auto kStereoTimeout = std::chrono::milliseconds(250);

enum Slot { kLeft = 0, kRight = 1, kScreen = 2, kSlots = 3 };

// --- game thread ------------------------------------------------------------

struct Frame {
  bool stereo;
  int eye;
  EyeView view;
};

std::mutex g_game_mutex;
int g_eye = kLeft;
bool g_have_view = false;
bool g_stereo = false;
EyeView g_view{};

// Latest eye poses from the XR thread.
std::mutex g_views_mutex;
std::array<EyeView, 2> g_latest{};
bool g_latest_valid = false;

// Frames queued at VdSwap, taken by the GPU thread at the matching swap.
std::mutex g_frames_mutex;
std::deque<Frame> g_frames;

// --- GPU thread -> XR thread ------------------------------------------------

struct Image {
  ComPtr<ID3D12Resource> texture;
  uint32_t width = 0, height = 0;  // part of the texture with the frame
  EyeView view{};
  Clock::time_point updated;
};
std::mutex g_images_mutex;
std::array<Image, kSlots> g_images;

std::atomic<float> g_hud_tan_x{0.0f}, g_hud_tan_y{0.0f};
std::atomic<int> g_lag{1};
std::atomic<int64_t> g_last_stereo_ns{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

bool StereoActive() {
  return NowNs() - g_last_stereo_ns.load(std::memory_order_relaxed) <
         std::chrono::duration_cast<std::chrono::nanoseconds>(kStereoTimeout).count();
}

// --- GPU thread ---------------------------------------------------------------

struct PresenterAccess : rex::ui::Presenter {
  using RefreshFn = bool (rex::ui::Presenter::*)(uint32_t, uint32_t, uint32_t,
                                                 std::function<bool(GuestOutputContext&)>, bool&);
  static RefreshFn Refresh() { return &PresenterAccess::RefreshGuestOutputImpl; }
};
using RefreshThunk = bool (*)(rex::ui::Presenter*, uint32_t, uint32_t, uint32_t,
                              std::function<bool(GuestOutputContext&)>, bool&);
RefreshThunk g_refresh = nullptr;

ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;

// The guest output image the presenter refreshed for the current swap.
ComPtr<ID3D12Resource> g_guest_output;
uint32_t g_guest_width = 0, g_guest_height = 0;

bool HookedRefresh(rex::ui::Presenter* self, uint32_t mailbox, uint32_t width, uint32_t height,
                   std::function<bool(GuestOutputContext&)> refresher, bool& is_8bpc) {
  auto wrapped = [inner = std::move(refresher), width, height](GuestOutputContext& context) {
    const bool ok = inner(context);
    if (ok) {
      auto& d3d12 = static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(
          context);
      g_guest_output = d3d12.resource_uav_capable();
      g_guest_width = width;
      g_guest_height = height;
    }
    return ok;
  };
  return g_refresh(self, mailbox, width, height, std::move(wrapped), is_8bpc);
}

// Copies guest output images on the direct queue.
struct Copier {
  static constexpr int kDepth = 3;
  ComPtr<ID3D12CommandAllocator> allocators[kDepth];
  std::vector<ComPtr<ID3D12Resource>> in_use[kDepth];
  uint64_t done_at[kDepth] = {};
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  uint64_t fence_value = 0;
  HANDLE event = nullptr;
  int next = 0;
  bool ready = false;
  bool failed = false;

  bool Init() {
    for (auto& allocator : allocators) {
      if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator)))) {
        return false;
      }
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           allocators[0].Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(list->Close()) ||
        FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
      return false;
    }
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return event != nullptr;
  }

  void Wait(uint64_t value) {
    if (fence->GetCompletedValue() < value) {
      fence->SetEventOnCompletion(value, event);
      WaitForSingleObject(event, 1000);
    }
  }
};
Copier g_copier;

ComPtr<ID3D12Resource> MakeTexture(const D3D12_RESOURCE_DESC& like, D3D12_RESOURCE_STATES state) {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = like.Width;
  desc.Height = like.Height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = like.Format;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  ComPtr<ID3D12Resource> texture;
  if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&texture)))) {
    return nullptr;
  }
  return texture;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}

// Textures read by the XR thread rest in this state.
constexpr D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

void CopyGuestOutput(const Frame& frame) {
  ComPtr<ID3D12Resource> source = std::move(g_guest_output);
  if (!source || g_copier.failed) {
    return;
  }
  if (!g_copier.ready) {
    if (!g_copier.Init()) {
      g_copier.failed = true;
      REXLOG_ERROR("VR: couldn't set up the frame copy");
      return;
    }
    g_copier.ready = true;
  }
  const int slot = frame.stereo ? frame.eye : kScreen;
  const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
  ComPtr<ID3D12Resource> target;
  {
    std::lock_guard lock(g_images_mutex);
    target = g_images[slot].texture;
  }
  bool fresh = false;
  if (!target || target->GetDesc().Width != source_desc.Width ||
      target->GetDesc().Height != source_desc.Height ||
      target->GetDesc().Format != source_desc.Format) {
    target = MakeTexture(source_desc, kShaderRead);
    if (!target) {
      g_copier.failed = true;
      REXLOG_ERROR("VR: couldn't create a {}x{} eye texture", source_desc.Width,
                   source_desc.Height);
      return;
    }
    fresh = true;
    REXLOG_INFO("VR: {} texture {}x{}", slot == kScreen ? "screen" : (slot ? "right" : "left"),
                source_desc.Width, source_desc.Height);
  }

  const int i = g_copier.next;
  g_copier.next = (i + 1) % Copier::kDepth;
  g_copier.Wait(g_copier.done_at[i]);
  g_copier.in_use[i].clear();
  g_copier.allocators[i]->Reset();
  auto* list = g_copier.list.Get();
  list->Reset(g_copier.allocators[i].Get(), nullptr);
  const D3D12_RESOURCE_STATES source_state =
      rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
  D3D12_RESOURCE_BARRIER before[] = {
      Transition(source.Get(), source_state, D3D12_RESOURCE_STATE_COPY_SOURCE),
      Transition(target.Get(), kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST)};
  list->ResourceBarrier(2, before);
  list->CopyResource(target.Get(), source.Get());
  D3D12_RESOURCE_BARRIER after[] = {
      Transition(source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, source_state),
      Transition(target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead)};
  list->ResourceBarrier(2, after);
  list->Close();
  ID3D12CommandList* lists[] = {list};
  g_queue->ExecuteCommandLists(1, lists);
  g_queue->Signal(g_copier.fence.Get(), ++g_copier.fence_value);
  g_copier.done_at[i] = g_copier.fence_value;
  g_copier.in_use[i] = {source, target};

  std::lock_guard lock(g_images_mutex);
  Image& image = g_images[slot];
  if (fresh) {
    image.texture = target;
  }
  image.width = g_guest_width ? g_guest_width : uint32_t(source_desc.Width);
  image.height = g_guest_height ? g_guest_height : source_desc.Height;
  image.view = frame.view;
  image.updated = Clock::now();
}

// --- XR thread ----------------------------------------------------------------

#define NHL3ON3_XR_FUNCTIONS(X)              \
  X(xrDestroyInstance)                       \
  X(xrGetInstanceProperties)                 \
  X(xrGetSystem)                             \
  X(xrGetSystemProperties)                   \
  X(xrGetD3D12GraphicsRequirementsKHR)       \
  X(xrCreateSession)                         \
  X(xrDestroySession)                        \
  X(xrPollEvent)                             \
  X(xrBeginSession)                          \
  X(xrEndSession)                            \
  X(xrEnumerateViewConfigurationViews)       \
  X(xrEnumerateSwapchainFormats)             \
  X(xrCreateSwapchain)                       \
  X(xrDestroySwapchain)                      \
  X(xrEnumerateSwapchainImages)              \
  X(xrAcquireSwapchainImage)                 \
  X(xrWaitSwapchainImage)                    \
  X(xrReleaseSwapchainImage)                 \
  X(xrCreateReferenceSpace)                  \
  X(xrWaitFrame)                             \
  X(xrBeginFrame)                            \
  X(xrEndFrame)                              \
  X(xrLocateViews)                           \
  X(xrResultToString)

struct Xr {
  PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr = nullptr;
  PFN_xrCreateInstance xrCreateInstance = nullptr;
#define X(name) PFN_##name name = nullptr;
  NHL3ON3_XR_FUNCTIONS(X)
#undef X
};
Xr xr;

XrInstance g_instance = XR_NULL_HANDLE;

std::string ResultName(XrResult result) {
  char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
  if (g_instance && xr.xrResultToString &&
      XR_SUCCEEDED(xr.xrResultToString(g_instance, result, buffer))) {
    return buffer;
  }
  return std::to_string(int(result));
}

bool Check(XrResult result, const char* what) {
  if (XR_FAILED(result)) {
    REXLOG_ERROR("VR: {} failed: {}", what, ResultName(result));
    return false;
  }
  return true;
}

// Draws a texture into a swapchain image with a full-screen triangle,
// converting the guest's gamma-encoded colour for sRGB swapchains.
constexpr char kBlitShader[] = R"(
cbuffer Constants : register(b0) { float2 uv_scale; float decode; float pad; };
Texture2D source : register(t0);
SamplerState linear_clamp : register(s0);
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
  V o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  o.uv = uv;
  return o;
}
float4 ps(V i) : SV_Target {
  float3 c = source.Sample(linear_clamp, i.uv * uv_scale).rgb;
  if (decode > 0.5) {
    c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
  }
  return float4(c, 1.0);
}
)";

struct Swapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  int32_t width = 0, height = 0;
  std::vector<ID3D12Resource*> images;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  UINT rtv_size = 0;
};

struct XrState {
  XrSystemId system = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  bool running = false;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool decode = false;
  Swapchain eyes[2];
  Swapchain screen;

  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> pipeline;
  ComPtr<ID3D12DescriptorHeap> srv_heap;
  UINT srv_size = 0;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  uint64_t fence_value = 0;
  HANDLE event = nullptr;
  std::array<ComPtr<ID3D12Resource>, kSlots> in_use;
};
XrState g_xr;

bool LoadOpenXr() {
  HMODULE loader = LoadLibraryW(L"openxr_loader.dll");
  if (!loader) {
    REXLOG_ERROR("VR: openxr_loader.dll not found next to the game");
    return false;
  }
  xr.xrGetInstanceProcAddr =
      reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
  if (!xr.xrGetInstanceProcAddr) {
    return false;
  }
  return XR_SUCCEEDED(xr.xrGetInstanceProcAddr(
      XR_NULL_HANDLE, "xrCreateInstance",
      reinterpret_cast<PFN_xrVoidFunction*>(&xr.xrCreateInstance)));
}

bool LoadInstanceFunctions() {
#define X(name)                                                                       \
  if (XR_FAILED(xr.xrGetInstanceProcAddr(g_instance, #name,                           \
                                         reinterpret_cast<PFN_xrVoidFunction*>(&xr.name)))) { \
    REXLOG_ERROR("VR: OpenXR runtime lacks " #name);                                  \
    return false;                                                                     \
  }
  NHL3ON3_XR_FUNCTIONS(X)
#undef X
  return true;
}

bool CreateBlitPipeline() {
  HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = compiler ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile"))
                          : nullptr;
  auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
      GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  if (!compile || !serialize) {
    REXLOG_ERROR("VR: d3dcompiler_47.dll or D3D12SerializeRootSignature unavailable");
    return false;
  }
  ComPtr<ID3DBlob> vs, ps, errors;
  if (FAILED(compile(kBlitShader, sizeof(kBlitShader) - 1, "vr_blit", nullptr, nullptr, "vs",
                     "vs_5_0", 0, 0, &vs, &errors)) ||
      FAILED(compile(kBlitShader, sizeof(kBlitShader) - 1, "vr_blit", nullptr, nullptr, "ps",
                     "ps_5_0", 0, 0, &ps, &errors))) {
    REXLOG_ERROR("VR: blit shader: {}",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    return false;
  }

  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &range;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 4;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root = {};
  root.NumParameters = 2;
  root.pParameters = params;
  root.NumStaticSamplers = 1;
  root.pStaticSamplers = &sampler;
  ComPtr<ID3DBlob> blob;
  if (FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
      FAILED(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&g_xr.root_signature)))) {
    REXLOG_ERROR("VR: blit root signature failed");
    return false;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
  pso.pRootSignature = g_xr.root_signature.Get();
  pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso.SampleMask = UINT_MAX;
  pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso.RasterizerState.DepthClipEnable = TRUE;
  pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso.NumRenderTargets = 1;
  pso.RTVFormats[0] = g_xr.format;
  pso.SampleDesc.Count = 1;
  if (FAILED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_xr.pipeline)))) {
    REXLOG_ERROR("VR: blit pipeline failed");
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap = {};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap.NumDescriptors = kSlots;
  heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_xr.srv_heap)))) {
    return false;
  }
  g_xr.srv_size = g_device->GetDescriptorHandleIncrementSize(heap.Type);
  if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&g_xr.allocator))) ||
      FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         g_xr.allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&g_xr.list))) ||
      FAILED(g_xr.list->Close()) ||
      FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_xr.fence)))) {
    return false;
  }
  g_xr.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return g_xr.event != nullptr;
}

bool CreateSwapchain(Swapchain& swapchain, int32_t width, int32_t height) {
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  info.format = g_xr.format;
  info.sampleCount = 1;
  info.width = uint32_t(width);
  info.height = uint32_t(height);
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  if (!Check(xr.xrCreateSwapchain(g_xr.session, &info, &swapchain.handle), "xrCreateSwapchain")) {
    return false;
  }
  swapchain.width = width;
  swapchain.height = height;
  uint32_t count = 0;
  xr.xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr);
  std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
  if (!Check(xr.xrEnumerateSwapchainImages(
                 swapchain.handle, count, &count,
                 reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
             "xrEnumerateSwapchainImages")) {
    return false;
  }
  D3D12_DESCRIPTOR_HEAP_DESC heap = {};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap.NumDescriptors = count;
  if (FAILED(g_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&swapchain.rtv_heap)))) {
    return false;
  }
  swapchain.rtv_size = g_device->GetDescriptorHandleIncrementSize(heap.Type);
  D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
  rtv.Format = g_xr.format;
  rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  D3D12_CPU_DESCRIPTOR_HANDLE handle = swapchain.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  for (auto& image : images) {
    swapchain.images.push_back(image.texture);
    g_device->CreateRenderTargetView(image.texture, &rtv, handle);
    handle.ptr += swapchain.rtv_size;
  }
  return true;
}

bool CreateSession() {
  XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
  if (!Check(xr.xrGetD3D12GraphicsRequirementsKHR(g_instance, g_xr.system, &requirements),
             "xrGetD3D12GraphicsRequirementsKHR")) {
    return false;
  }
  const LUID luid = g_device->GetAdapterLuid();
  if (luid.LowPart != requirements.adapterLuid.LowPart ||
      luid.HighPart != requirements.adapterLuid.HighPart) {
    REXLOG_WARN("VR: the headset runtime wants a different GPU than the game is using");
  }
  XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
  binding.device = g_device;
  binding.queue = g_queue;
  XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
  info.next = &binding;
  info.systemId = g_xr.system;
  if (!Check(xr.xrCreateSession(g_instance, &info, &g_xr.session), "xrCreateSession")) {
    return false;
  }
  XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  space.poseInReferenceSpace.orientation.w = 1.0f;
  if (!Check(xr.xrCreateReferenceSpace(g_xr.session, &space, &g_xr.local),
             "xrCreateReferenceSpace")) {
    return false;
  }

  uint32_t count = 0;
  xr.xrEnumerateSwapchainFormats(g_xr.session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  xr.xrEnumerateSwapchainFormats(g_xr.session, count, &count, formats.data());
  for (DXGI_FORMAT preferred : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
    if (std::find(formats.begin(), formats.end(), int64_t(preferred)) != formats.end()) {
      g_xr.format = preferred;
      break;
    }
  }
  if (g_xr.format == DXGI_FORMAT_UNKNOWN) {
    REXLOG_ERROR("VR: the runtime offers no 8-bit RGBA swapchain format");
    return false;
  }
  g_xr.decode = g_xr.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                g_xr.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

  std::array<XrViewConfigurationView, 2> views{};
  for (auto& view : views) {
    view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
  }
  count = 2;
  if (!Check(xr.xrEnumerateViewConfigurationViews(g_instance, g_xr.system,
                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                   &count, views.data()),
             "xrEnumerateViewConfigurationViews") ||
      !CreateBlitPipeline()) {
    return false;
  }
  for (int eye = 0; eye < 2; ++eye) {
    if (!CreateSwapchain(g_xr.eyes[eye], int32_t(views[eye].recommendedImageRectWidth),
                         int32_t(views[eye].recommendedImageRectHeight))) {
      return false;
    }
  }
  if (!CreateSwapchain(g_xr.screen, 1920, 1080)) {
    return false;
  }
  REXLOG_INFO("VR: session ready, eyes {}x{}, swapchain format {}{}",
              views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight,
              int(g_xr.format), g_xr.decode ? " (sRGB)" : "");
  return true;
}

void HandleEvents(bool& quit) {
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xr.xrPollEvent(g_instance, &event) == XR_SUCCESS) {
    if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
      g_xr.state = changed.state;
      REXLOG_INFO("VR: session state {}", int(changed.state));
      if (changed.state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        g_xr.running = Check(xr.xrBeginSession(g_xr.session, &begin), "xrBeginSession");
      } else if (changed.state == XR_SESSION_STATE_STOPPING) {
        xr.xrEndSession(g_xr.session);
        g_xr.running = false;
      } else if (changed.state == XR_SESSION_STATE_EXITING ||
                 changed.state == XR_SESSION_STATE_LOSS_PENDING) {
        g_xr.running = false;
        quit = true;
      }
    } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      g_xr.running = false;
      quit = true;
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

EyeView ToEyeView(const XrView& view) {
  EyeView eye{};
  eye.position[0] = view.pose.position.x;
  eye.position[1] = view.pose.position.y;
  eye.position[2] = view.pose.position.z;
  eye.orientation[0] = view.pose.orientation.x;
  eye.orientation[1] = view.pose.orientation.y;
  eye.orientation[2] = view.pose.orientation.z;
  eye.orientation[3] = view.pose.orientation.w;
  eye.tan_x = std::max(std::tan(-view.fov.angleLeft), std::tan(view.fov.angleRight));
  eye.tan_y = std::max(std::tan(view.fov.angleUp), std::tan(-view.fov.angleDown));
  return eye;
}

XrPosef ToPose(const EyeView& eye) {
  XrPosef pose;
  pose.position = {eye.position[0], eye.position[1], eye.position[2]};
  pose.orientation = {eye.orientation[0], eye.orientation[1], eye.orientation[2],
                      eye.orientation[3]};
  return pose;
}

void Blit(Swapchain& swapchain, uint32_t image_index, int slot, const Image& image) {
  auto* list = g_xr.list.Get();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = image.texture->GetDesc().Format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_xr.srv_heap->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += SIZE_T(slot) * g_xr.srv_size;
  g_device->CreateShaderResourceView(image.texture.Get(), &srv, cpu);
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_xr.srv_heap->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += UINT64(slot) * g_xr.srv_size;

  D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  rtv.ptr += SIZE_T(image_index) * swapchain.rtv_size;
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport = {0.0f, 0.0f, float(swapchain.width), float(swapchain.height),
                                   0.0f, 1.0f};
  const D3D12_RECT scissor = {0, 0, swapchain.width, swapchain.height};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->SetGraphicsRootDescriptorTable(0, gpu);
  const D3D12_RESOURCE_DESC desc = image.texture->GetDesc();
  const float constants[4] = {float(image.width) / float(desc.Width),
                              float(image.height) / float(desc.Height), g_xr.decode ? 1.0f : 0.0f,
                              0.0f};
  list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
  list->DrawInstanced(3, 1, 0, 0);
}

bool Acquire(Swapchain& swapchain, uint32_t& index) {
  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xr.xrAcquireSwapchainImage(swapchain.handle, &acquire, &index))) {
    return false;
  }
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  return XR_SUCCEEDED(xr.xrWaitSwapchainImage(swapchain.handle, &wait));
}

void Release(Swapchain& swapchain) {
  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xr.xrReleaseSwapchainImage(swapchain.handle, &release);
}

void RunFrame() {
  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  if (!Check(xr.xrWaitFrame(g_xr.session, &wait_info, &frame), "xrWaitFrame")) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return;
  }
  static XrDuration logged_period = 0;
  if (frame.predictedDisplayPeriod > 0 &&
      std::llabs(frame.predictedDisplayPeriod - logged_period) > 200000) {
    logged_period = frame.predictedDisplayPeriod;
    REXLOG_INFO("VR: headset refresh {:.1f} Hz (each eye gets every other game frame)",
                1e9 / double(logged_period));
  }
  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  if (XR_FAILED(xr.xrBeginFrame(g_xr.session, &begin_info))) {
    return;
  }

  XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
  locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate.displayTime = frame.predictedDisplayTime;
  locate.space = g_xr.local;
  XrViewState view_state{XR_TYPE_VIEW_STATE};
  std::array<XrView, 2> views{};
  views[0].type = views[1].type = XR_TYPE_VIEW;
  uint32_t view_count = 0;
  if (XR_SUCCEEDED(xr.xrLocateViews(g_xr.session, &locate, &view_state, 2, &view_count,
                                    views.data())) &&
      view_count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
      (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
    std::lock_guard lock(g_views_mutex);
    g_latest = {ToEyeView(views[0]), ToEyeView(views[1])};
    g_latest_valid = true;
  }

  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  std::array<XrCompositionLayerProjectionView, 2> projection_views{};
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  const XrCompositionLayerBaseHeader* layers[1] = {};
  uint32_t layer_count = 0;

  if (frame.shouldRender) {
    std::array<Image, kSlots> images;
    {
      std::lock_guard lock(g_images_mutex);
      images = g_images;
    }
    const auto now = Clock::now();
    const bool stereo = images[kLeft].texture && images[kRight].texture && StereoActive() &&
                        now - images[kLeft].updated < kStereoTimeout &&
                        now - images[kRight].updated < kStereoTimeout;
    const bool screen = !stereo && images[kScreen].texture;
    if (stereo || screen) {
      if (g_xr.fence->GetCompletedValue() < g_xr.fence_value) {
        g_xr.fence->SetEventOnCompletion(g_xr.fence_value, g_xr.event);
        WaitForSingleObject(g_xr.event, 1000);
      }
      g_xr.allocator->Reset();
      auto* list = g_xr.list.Get();
      list->Reset(g_xr.allocator.Get(), g_xr.pipeline.Get());
      list->SetGraphicsRootSignature(g_xr.root_signature.Get());
      ID3D12DescriptorHeap* heaps[] = {g_xr.srv_heap.Get()};
      list->SetDescriptorHeaps(1, heaps);
      list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      std::vector<Swapchain*> acquired;
      bool ok = true;
      if (stereo) {
        for (int eye = 0; eye < 2 && ok; ++eye) {
          uint32_t index = 0;
          ok = Acquire(g_xr.eyes[eye], index);
          if (ok) {
            acquired.push_back(&g_xr.eyes[eye]);
            Blit(g_xr.eyes[eye], index, eye, images[eye]);
            const EyeView& drawn = images[eye].view;
            auto& view = projection_views[eye];
            view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            view.pose = ToPose(drawn);
            view.fov = {-std::atan(drawn.tan_x), std::atan(drawn.tan_x), std::atan(drawn.tan_y),
                        -std::atan(drawn.tan_y)};
            view.subImage.swapchain = g_xr.eyes[eye].handle;
            view.subImage.imageRect = {{0, 0}, {g_xr.eyes[eye].width, g_xr.eyes[eye].height}};
          }
        }
      } else {
        uint32_t index = 0;
        ok = Acquire(g_xr.screen, index);
        if (ok) {
          acquired.push_back(&g_xr.screen);
          Blit(g_xr.screen, index, kScreen, images[kScreen]);
        }
      }
      list->Close();
      ID3D12CommandList* lists[] = {list};
      g_queue->ExecuteCommandLists(1, lists);
      g_queue->Signal(g_xr.fence.Get(), ++g_xr.fence_value);
      for (int slot = 0; slot < kSlots; ++slot) {
        g_xr.in_use[slot] = images[slot].texture;
      }
      for (Swapchain* swapchain : acquired) {
        Release(*swapchain);
      }
      if (ok && stereo) {
        projection.space = g_xr.local;
        projection.viewCount = 2;
        projection.views = projection_views.data();
        layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
      } else if (ok) {
        const float width = float(REXCVAR_GET(vr_screen_width));
        quad.space = g_xr.local;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_xr.screen.handle;
        quad.subImage.imageRect = {{0, 0}, {g_xr.screen.width, g_xr.screen.height}};
        quad.pose.orientation.w = 1.0f;
        quad.pose.position = {0.0f, 0.0f, -float(REXCVAR_GET(vr_screen_distance))};
        quad.size = {width, width * 9.0f / 16.0f};
        layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
      }
    }
  }

  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = frame.predictedDisplayTime;
  end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end_info.layerCount = layer_count;
  end_info.layers = layers;
  Check(xr.xrEndFrame(g_xr.session, &end_info), "xrEndFrame");
}

void XrThread() {
  if (!LoadOpenXr()) {
    return;
  }
  const char* extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  std::snprintf(create.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE,
                "NHL 3on3");
  create.applicationInfo.applicationVersion = 1;
  std::snprintf(create.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "ReXGlue");
  create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  create.enabledExtensionCount = 1;
  create.enabledExtensionNames = extensions;
  const XrResult created = xr.xrCreateInstance(&create, &g_instance);
  if (XR_FAILED(created)) {
    REXLOG_ERROR("VR: no OpenXR runtime ({}); is the headset connected and an OpenXR runtime "
                 "set active?",
                 int(created));
    return;
  }
  if (!LoadInstanceFunctions()) {
    return;
  }
  XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
  xr.xrGetInstanceProperties(g_instance, &properties);
  REXLOG_INFO("VR: OpenXR runtime {}", properties.runtimeName);

  XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO};
  system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  while (true) {
    const XrResult result = xr.xrGetSystem(g_instance, &system, &g_xr.system);
    if (XR_SUCCEEDED(result)) {
      break;
    }
    if (result != XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
      Check(result, "xrGetSystem");
      return;
    }
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("VR: waiting for the headset");
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
  xr.xrGetSystemProperties(g_instance, g_xr.system, &system_properties);
  REXLOG_INFO("VR: headset {}", system_properties.systemName);
  if (!CreateSession()) {
    return;
  }
  bool quit = false;
  bool f9_down = false;
  while (!quit) {
    HandleEvents(quit);
    const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !f9_down) {
      const int lag = (g_lag.load() + 1) % 3;
      g_lag.store(lag);
      REXLOG_INFO("VR: frame lag {}", lag);
    }
    f9_down = f9;
    if (g_xr.running) {
      RunFrame();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  REXLOG_INFO("VR: session ended");
  {
    std::lock_guard lock(g_views_mutex);
    g_latest_valid = false;
  }
}

}  // namespace

namespace nhl3on3::vr {

bool Enabled() {
  static const bool enabled = REXCVAR_GET(vr);
  return enabled;
}

void TableTransform(float& scale, float centre[3]) {
  scale = float(std::clamp(REXCVAR_GET(vr_scale), 0.0005, 1.0));
  centre[0] = 0.0f;
  centre[1] = -float(REXCVAR_GET(vr_table_height));
  centre[2] = -float(REXCVAR_GET(vr_table_distance));
}

bool FrameEye(EyeView& out) {
  std::lock_guard lock(g_game_mutex);
  if (!g_have_view) {
    std::lock_guard views(g_views_mutex);
    if (!g_latest_valid) {
      return false;
    }
    g_view = g_latest[g_eye];
    g_have_view = true;
  }
  out = g_view;
  return true;
}

void NoteStereoFrame() {
  std::lock_guard lock(g_game_mutex);
  g_stereo = true;
}

void OnGuestSwap() {
  std::lock_guard lock(g_game_mutex);
  {
    std::lock_guard frames(g_frames_mutex);
    g_frames.push_back({g_stereo && g_have_view, g_eye, g_view});
    while (g_frames.size() > 8) {
      g_frames.pop_front();
    }
  }
  g_stereo = false;
  g_eye ^= 1;
  std::lock_guard views(g_views_mutex);
  g_have_view = g_latest_valid;
  if (g_have_view) {
    g_view = g_latest[g_eye];
  }
}

void InstallPresenterHook() {
  auto* runtime = rex::Runtime::instance();
  auto* system =
      runtime ? static_cast<rex::graphics::GraphicsSystem*>(runtime->graphics_system()) : nullptr;
  auto* provider =
      system ? dynamic_cast<rex::ui::d3d12::D3D12Provider*>(system->provider()) : nullptr;
  auto* presenter = system ? system->presenter() : nullptr;
  if (!provider || !presenter) {
    REXLOG_ERROR("VR: needs the Direct3D 12 renderer; VR is off");
    return;
  }
  g_device = provider->GetDevice();
  g_queue = provider->GetDirectQueue();
  if (!PatchVirtual(presenter, VtableOffset(PresenterAccess::Refresh()), &HookedRefresh,
                    g_refresh)) {
    REXLOG_ERROR("VR: couldn't hook the presenter; VR is off");
    return;
  }
  g_lag.store(std::clamp(REXCVAR_GET(vr_frame_lag), 0, 2));
  REXLOG_INFO("VR: on, starting OpenXR (frame lag {}; F9 cycles it)", g_lag.load());
  std::thread(XrThread).detach();
}

void OnHostSwap() {
  if (!g_refresh) {
    return;
  }
  // Swaps queued so far, newest first; the frame shown now was drawn with the
  // camera of the one `lag` swaps back.
  static std::deque<Frame> history;
  {
    std::lock_guard lock(g_frames_mutex);
    if (g_frames.empty()) {
      g_guest_output.Reset();
      return;
    }
    history.push_front(g_frames.front());
    g_frames.pop_front();
  }
  while (history.size() > 4) {
    history.pop_back();
  }
  const size_t lag = size_t(g_lag.load(std::memory_order_relaxed));
  if (lag >= history.size()) {
    g_guest_output.Reset();
    return;
  }
  const Frame frame = history[lag];
  if (frame.stereo) {
    g_last_stereo_ns.store(NowNs(), std::memory_order_relaxed);
    g_hud_tan_x.store(frame.view.tan_x, std::memory_order_relaxed);
    g_hud_tan_y.store(frame.view.tan_y, std::memory_order_relaxed);
  }
  CopyGuestOutput(frame);
}

void HudSqueeze(float& x, float& y) {
  x = y = 1.0f;
  const float tan_x = g_hud_tan_x.load(std::memory_order_relaxed);
  const float tan_y = g_hud_tan_y.load(std::memory_order_relaxed);
  if (!StereoActive() || tan_x <= 0.0f || tan_y <= 0.0f) {
    return;
  }
  // The frame spans the eye's view window, so 16:9 HUD proportions need
  // y = x * (tan_x / tan_y) * 9 / 16.
  x = float(std::clamp(REXCVAR_GET(vr_hud_scale), 0.1, 1.0));
  y = std::min(1.0f, x * tan_x / tan_y * 9.0f / 16.0f);
}

}  // namespace nhl3on3::vr

#endif
