#pragma once

#include <cstdint>

// Controllers streamed from other machines (tools/remote_pad.py). Each one
// drives a guest user index while its packets keep arriving.
namespace nhl3on3::remote_pad {

// Whether a remote controller currently drives `user_index`.
bool Drives(uint32_t user_index);

// Fills a guest X_INPUT_STATE (may be 0). Returns false if no remote
// controller drives `user_index`.
bool GetState(uint8_t* base, uint32_t user_index, uint32_t state);

// Fills a guest X_INPUT_CAPABILITIES (may be 0).
bool GetCapabilities(uint8_t* base, uint32_t user_index, uint32_t caps);

// Pops the next button event for `user_index`, or for any remote controller
// when `any_user` is set, into a guest X_INPUT_KEYSTROKE. Returns the user
// index it came from, or -1 if none is queued.
int32_t PopKeystroke(uint8_t* base, uint32_t user_index, bool any_user, uint32_t keystroke);

}  // namespace nhl3on3::remote_pad
