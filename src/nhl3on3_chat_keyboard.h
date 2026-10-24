// The PC keyboard types into the game's on-screen keyboard (lobby chat and
// other text entry) as an Xbox 360 chatpad would.

#pragma once

#include <cstdint>
#include <string_view>

namespace rex::ui {
class Window;
class WindowedAppContext;
}  // namespace rex::ui

namespace nhl3on3::chat_keyboard {

// Starts listening to the window's keyboard. UI thread.
void Attach(rex::ui::WindowedAppContext& context, rex::ui::Window* window);

// Every front-end trace() line; the on-screen keyboard reports being shown
// and hidden through them.
void NoteUiTrace(std::string_view text);

// While the on-screen keyboard is up, keyboard queries (XINPUT_FLAG_KEYBOARD)
// see a chatpad on every controller. Fills X_INPUT_CAPABILITIES.
bool GetCapabilities(uint8_t* base, uint32_t capabilities);

// Fills X_INPUT_KEYSTROKE with the next typed key; false when there is none.
bool PopKeystroke(uint8_t* base, uint32_t keystroke, uint8_t user_index);

}  // namespace nhl3on3::chat_keyboard
