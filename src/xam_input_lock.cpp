// Serializes XamInput* calls. The SDK's InputSystem::RefreshDevices() mutates
// its device list without locking, and this game polls input from several
// guest threads, which corrupts the heap.
//
// Remote controllers (remote_pad_port) take over the slot they name. Otherwise
// player_slot applies: the guest user index it names and user 0 swap places,
// so the first physical controller plays as that user.

#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/ppc/func.h>

#include "nhl3on3_display.h"
#include "nhl3on3_remote_pad.h"
#include "runtime_export.h"

REXCVAR_DEFINE_INT32(player_slot, 0, "NHL3on3/Input",
                     "Local player slot (0-3) the first controller plays as; whatever "
                     "would be on that slot moves to slot 0")
    .range(0, 3);

namespace {

namespace remote_pad = nhl3on3::remote_pad;

std::mutex g_input_mutex;

constexpr uint32_t kUserIndexAny = 0xFF;
constexpr uint32_t kFlagAnyUser = 1u << 30;
constexpr uint32_t kErrorEmpty = 0x10D2;
constexpr uint32_t kKeystrokeUserIndexOffset = 6;

uint32_t SwapSlot(uint32_t user_index) {
  const uint32_t slot = uint32_t(REXCVAR_GET(player_slot));
  if (user_index == 0) {
    return slot;
  }
  return user_index == slot ? 0 : user_index;
}

bool IsAnyUser(uint32_t user_index, uint32_t flags) {
  return (user_index & 0xFF) == kUserIndexAny || (flags & kFlagAnyUser);
}

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// Forwards a (user_index, ...) call for a physical controller.
void CallPhysical(const nhl3on3::GuestFunc& original, PPCContext& ctx, uint8_t* base) {
  if ((ctx.r3.u32 & 0xFF) != kUserIndexAny) {
    ctx.r3.u64 = SwapSlot(ctx.r3.u32);
  }
  original(ctx, base);
}

}  // namespace

// (user_index, flags, X_INPUT_CAPABILITIES*)
extern "C" REX_FUNC(__imp__XamInputGetCapabilities) {
  static const auto original = nhl3on3::ResolveRuntimeExport("__imp__XamInputGetCapabilities");
  std::lock_guard lock(g_input_mutex);
  if (!IsAnyUser(ctx.r3.u32, ctx.r4.u32) &&
      remote_pad::GetCapabilities(base, ctx.r3.u32, ctx.r5.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  CallPhysical(original, ctx, base);
}

// (user_index, flags, X_INPUT_STATE*)
extern "C" REX_FUNC(__imp__XamInputGetState) {
  static const auto original = nhl3on3::ResolveRuntimeExport("__imp__XamInputGetState");
  nhl3on3::NotePadRead();
  std::lock_guard lock(g_input_mutex);
  if (!IsAnyUser(ctx.r3.u32, ctx.r4.u32) && remote_pad::GetState(base, ctx.r3.u32, ctx.r5.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  CallPhysical(original, ctx, base);
}

// (user_index, reserved, X_INPUT_VIBRATION*). Remote controllers have no rumble.
extern "C" REX_FUNC(__imp__XamInputSetState) {
  static const auto original = nhl3on3::ResolveRuntimeExport("__imp__XamInputSetState");
  std::lock_guard lock(g_input_mutex);
  if ((ctx.r3.u32 & 0xFF) != kUserIndexAny && remote_pad::Drives(ctx.r3.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  CallPhysical(original, ctx, base);
}

// (DWORD* user_index, flags, X_INPUT_KEYSTROKE*). On success the user index is
// written back through the pointer and into the keystroke.
extern "C" REX_FUNC(__imp__XamInputGetKeystrokeEx) {
  static const auto original = nhl3on3::ResolveRuntimeExport("__imp__XamInputGetKeystrokeEx");
  std::lock_guard lock(g_input_mutex);
  uint8_t* user_index = ctx.r3.u32 ? base + ctx.r3.u32 : nullptr;
  const uint32_t flags = ctx.r4.u32, keystroke = ctx.r5.u32;
  const uint32_t requested = user_index ? LoadBE32(user_index) : kUserIndexAny;
  const bool any_user = IsAnyUser(requested, flags);

  const int32_t remote = remote_pad::PopKeystroke(base, requested, any_user, keystroke);
  if (remote >= 0) {
    if (user_index) {
      StoreBE32(user_index, uint32_t(remote));
    }
    ctx.r3.u64 = 0;
    return;
  }
  if (!any_user && remote_pad::Drives(requested)) {
    ctx.r3.u64 = kErrorEmpty;
    return;
  }

  if (user_index && (requested & 0xFF) != kUserIndexAny) {
    StoreBE32(user_index, SwapSlot(requested));
  }
  original(ctx, base);
  if (ctx.r3.u32 != 0) {
    if (user_index) {
      StoreBE32(user_index, requested);
    }
    return;
  }
  if (keystroke) {
    uint8_t& keystroke_user = base[keystroke + kKeystrokeUserIndexOffset];
    keystroke_user = uint8_t(SwapSlot(keystroke_user));
    if (user_index) {
      StoreBE32(user_index, keystroke_user);
    }
  }
}
