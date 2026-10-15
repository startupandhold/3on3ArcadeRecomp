// Traces the game's save module (0x829F8000-0x82A10000): the storage device
// selector (XamShowDeviceSelectorUI) and save creation (XamContentCreateEx)
// only run from here, and in-game options are only kept once they have.
// Each function logs its first few calls; sub_82A03708(module, state) is the
// module's state change and logs more of them.

#include <atomic>
#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#define NHL3ON3_SAVE_HOOK(addr, limit)                                                    \
  extern "C" void __imp__sub_##addr(PPCContext& __restrict ctx, uint8_t* base);           \
  extern "C" REX_FUNC(sub_##addr) {                                                       \
    static std::atomic<int> calls{0};                                                     \
    const int n = calls++;                                                                \
    const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;                     \
    const uint32_t lr = uint32_t(ctx.lr);                                                 \
    __imp__sub_##addr(ctx, base);                                                         \
    if (n < (limit)) {                                                                    \
      REXLOG_INFO("[save] sub_" #addr "({:X}, {:X}, {:X}) = {:X} from {:08X} [call {}]", r3, \
                  r4, r5, ctx.r3.u32, lr, n + 1);                                         \
    }                                                                                     \
  }

NHL3ON3_SAVE_HOOK(829F8378, 8)
NHL3ON3_SAVE_HOOK(82970B18, 8)
NHL3ON3_SAVE_HOOK(82A00080, 4)
NHL3ON3_SAVE_HOOK(82A00138, 4)
NHL3ON3_SAVE_HOOK(82A001C0, 4)
NHL3ON3_SAVE_HOOK(82A00238, 4)
NHL3ON3_SAVE_HOOK(82A002B8, 4)
NHL3ON3_SAVE_HOOK(82A003B0, 4)
NHL3ON3_SAVE_HOOK(82A00418, 4)
NHL3ON3_SAVE_HOOK(82A00478, 4)
NHL3ON3_SAVE_HOOK(82A005E8, 4)
NHL3ON3_SAVE_HOOK(82A00680, 4)
NHL3ON3_SAVE_HOOK(82A006D8, 4)
NHL3ON3_SAVE_HOOK(82A007D0, 4)
NHL3ON3_SAVE_HOOK(82A00828, 4)
NHL3ON3_SAVE_HOOK(82A00880, 4)
NHL3ON3_SAVE_HOOK(82A00938, 4)
NHL3ON3_SAVE_HOOK(82A00A28, 8)
NHL3ON3_SAVE_HOOK(82A00AD0, 4)
NHL3ON3_SAVE_HOOK(82A00B18, 4)
NHL3ON3_SAVE_HOOK(82A00BB8, 4)
NHL3ON3_SAVE_HOOK(82A00C08, 4)
NHL3ON3_SAVE_HOOK(82A00C78, 4)
NHL3ON3_SAVE_HOOK(82A00C80, 4)
NHL3ON3_SAVE_HOOK(82A00DF8, 4)
NHL3ON3_SAVE_HOOK(82A00E60, 4)
NHL3ON3_SAVE_HOOK(82A00EA8, 4)
NHL3ON3_SAVE_HOOK(82A00EC8, 4)
NHL3ON3_SAVE_HOOK(82A00EF8, 4)
NHL3ON3_SAVE_HOOK(82A00F28, 4)
NHL3ON3_SAVE_HOOK(82A00F58, 4)
NHL3ON3_SAVE_HOOK(82A00FD0, 4)
NHL3ON3_SAVE_HOOK(82A03708, 64)
NHL3ON3_SAVE_HOOK(82A03728, 8)
NHL3ON3_SAVE_HOOK(82A037D8, 8)
NHL3ON3_SAVE_HOOK(82A0FB60, 8)
