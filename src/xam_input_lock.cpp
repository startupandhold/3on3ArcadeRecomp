// Serializes XamInput* calls. The SDK's InputSystem::RefreshDevices() mutates
// its device list without locking, and this game polls input from several
// guest threads, which corrupts the heap.
//
// Also applies player_slot: the guest user index it names and user 0 swap
// places, so the first physical controller plays as that user.

#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/ppc/func.h>

#include "runtime_export.h"

REXCVAR_DEFINE_INT32(player_slot, 0, "NHL3on3/Input",
                     "Local player slot (0-3) the first controller plays as; whatever "
                     "would be on that slot moves to slot 0")
    .range(0, 3);

namespace {

std::mutex g_input_mutex;

constexpr uint32_t kUserIndexAny = 0xFF;
constexpr uint32_t kKeystrokeUserIndexOffset = 6;

uint32_t SwapSlot(uint32_t user_index) {
  const uint32_t slot = uint32_t(REXCVAR_GET(player_slot));
  if (user_index == 0) {
    return slot;
  }
  return user_index == slot ? 0 : user_index;
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

}  // namespace

// (user_index, ...): the first argument is the guest user index.
#define NHL3ON3_LOCKED_XAM_INPUT(name)                                                  \
  extern "C" REX_FUNC(name) {                                                           \
    static const nhl3on3::GuestFunc original = nhl3on3::ResolveRuntimeExport(#name);    \
    std::lock_guard lock(g_input_mutex);                                                \
    if ((ctx.r3.u32 & 0xFF) != kUserIndexAny) {                                         \
      ctx.r3.u64 = SwapSlot(ctx.r3.u32);                                                \
    }                                                                                   \
    original(ctx, base);                                                                \
  }

NHL3ON3_LOCKED_XAM_INPUT(__imp__XamInputGetCapabilities)
NHL3ON3_LOCKED_XAM_INPUT(__imp__XamInputGetState)
NHL3ON3_LOCKED_XAM_INPUT(__imp__XamInputSetState)

// (DWORD* user_index, flags, X_INPUT_KEYSTROKE*). On success the runtime writes
// the physical user back through the pointer and into the keystroke.
extern "C" REX_FUNC(__imp__XamInputGetKeystrokeEx) {
  static const nhl3on3::GuestFunc original =
      nhl3on3::ResolveRuntimeExport("__imp__XamInputGetKeystrokeEx");
  std::lock_guard lock(g_input_mutex);
  uint8_t* user_index = ctx.r3.u32 ? base + ctx.r3.u32 : nullptr;
  const uint32_t keystroke = ctx.r5.u32;
  const uint32_t requested = user_index ? LoadBE32(user_index) : kUserIndexAny;
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
