// Serializes XamInput* calls. The SDK's InputSystem::RefreshDevices() mutates
// its device list without locking, and this game polls input from several
// guest threads, which corrupts the heap.

#include <mutex>

#include <rex/ppc/func.h>

#include "runtime_export.h"

namespace {
std::mutex g_input_mutex;
}  // namespace

#define NHL3ON3_LOCKED_XAM_INPUT(name)                                                  \
  extern "C" REX_FUNC(name) {                                                           \
    static const nhl3on3::GuestFunc original = nhl3on3::ResolveRuntimeExport(#name);    \
    std::lock_guard lock(g_input_mutex);                                                \
    original(ctx, base);                                                                \
  }

NHL3ON3_LOCKED_XAM_INPUT(__imp__XamInputGetCapabilities)
NHL3ON3_LOCKED_XAM_INPUT(__imp__XamInputGetState)
NHL3ON3_LOCKED_XAM_INPUT(__imp__XamInputSetState)
NHL3ON3_LOCKED_XAM_INPUT(__imp__XamInputGetKeystrokeEx)
