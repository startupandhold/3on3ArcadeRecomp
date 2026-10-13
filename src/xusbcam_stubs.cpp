// Xbox Live Vision camera imports. ReXGlue 0.10.0 leaves xboxkrnl_usbcam.cpp
// out of the runtime build, so the two the game imports are provided here.
// The camera always reports "not connected".

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace nhl3on3 {

using rex::X_RESULT;

u32 XUsbcamGetState_entry() {
  return 0;
}

}  // namespace nhl3on3

using rex::X_RESULT;

REX_EXPORT(__imp__XUsbcamGetState, nhl3on3::XUsbcamGetState_entry)
REX_EXPORT_STUB_RETURN(__imp__XUsbcamSetConfig, X_ERROR_DEVICE_NOT_CONNECTED);
