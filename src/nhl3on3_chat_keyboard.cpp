// The game polls each controller for chatpad keystrokes (XamInputGetKeystrokeEx
// with XINPUT_FLAG_KEYBOARD) and hands them to the on-screen keyboard
// (stdvirkb): Backspace, Enter, Shift, Caps Lock, Escape and Space by virtual
// key, anything else by its character. While that keyboard is shown, keys
// typed on the PC are queued here as such keystrokes, and kept from the
// keyboard-as-controller emulation.

#include "nhl3on3_chat_keyboard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

namespace nhl3on3::chat_keyboard {
namespace {

using rex::ui::VirtualKey;

constexpr uint8_t kDevTypeKeyboard = 2;
constexpr uint16_t kKeystrokeKeyDown = 0x0001;
constexpr size_t kMaxQueued = 64;
// The game keeps one typed character until the keyboard has taken it, and
// drops any keystroke it polls meanwhile; it polls and consumes once a frame.
constexpr auto kKeystrokeInterval = std::chrono::milliseconds(40);

struct Keystroke {
  uint16_t virtual_key;
  uint16_t unicode;
};

std::atomic<bool> g_shown{false};
std::mutex g_mutex;
std::deque<Keystroke> g_queue;
std::chrono::steady_clock::time_point g_last_pop;
rex::ui::WindowedAppContext* g_context = nullptr;
rex::ui::Window* g_window = nullptr;

void StoreBE16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

void Queue(uint16_t virtual_key, uint16_t unicode) {
  std::lock_guard lock(g_mutex);
  if (g_queue.size() < kMaxQueued) {
    g_queue.push_back({virtual_key, unicode});
  }
}

class Listener : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!g_shown || e.is_alt_pressed() || e.is_ctrl_pressed()) {
      return;
    }
    const auto key = e.virtual_key();
    if (key >= VirtualKey::kF1 && key <= VirtualKey::kF24) {
      return;
    }
    if (key == VirtualKey::kBack || key == VirtualKey::kReturn || key == VirtualKey::kEscape) {
      Queue(uint16_t(key), 0);
    }
    // Characters arrive as OnKeyChar.
    e.set_handled(true);
  }

  void OnKeyChar(rex::ui::KeyEvent& e) override {
    if (!g_shown) {
      return;
    }
    const uint32_t c = uint32_t(e.virtual_key());
    if (c < 0x20 || c > 0xFFFF || c == 0x7F) {
      return;
    }
    // Only Space goes by virtual key; 0 keeps a character from being read as
    // one of the function keys.
    Queue(c == ' ' ? uint16_t(VirtualKey::kSpace) : 0, uint16_t(c));
    e.set_handled(true);
  }
};

Listener g_listener;

void SetTextInput(bool active) {
  if (!g_context) {
    return;
  }
  g_context->CallInUIThread([active] {
    if (g_window) {
      g_window->SetTextInputActive(active);
    }
  });
}

}  // namespace

void Attach(rex::ui::WindowedAppContext& context, rex::ui::Window* window) {
  if (!window) {
    return;
  }
  g_context = &context;
  g_window = window;
  // Ahead of the controller emulation and the app's own binds.
  window->AddInputListener(&g_listener, 1000);
}

void NoteUiTrace(std::string_view text) {
  bool shown;
  if (text.starts_with("showKeyBoardGui(")) {
    shown = true;
  } else if (text.starts_with("hideKeyBoardGui()")) {
    shown = false;
  } else {
    return;
  }
  if (g_shown.exchange(shown) == shown) {
    return;
  }
  if (!shown) {
    std::lock_guard lock(g_mutex);
    g_queue.clear();
  }
  SetTextInput(shown);
  REXLOG_INFO("[ui] on-screen keyboard {}; PC keyboard typing {}", shown ? "shown" : "hidden",
              shown ? "on" : "off");
}

bool GetCapabilities(uint8_t* base, uint32_t capabilities) {
  if (!g_shown || !capabilities) {
    return false;
  }
  uint8_t* caps = base + capabilities;
  std::fill(caps, caps + 20, uint8_t(0));
  caps[0] = kDevTypeKeyboard;
  return true;
}

bool PopKeystroke(uint8_t* base, uint32_t keystroke, uint8_t user_index) {
  if (!g_shown || !keystroke) {
    return false;
  }
  std::lock_guard lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  if (g_queue.empty() || now - g_last_pop < kKeystrokeInterval) {
    return false;
  }
  g_last_pop = now;
  const Keystroke event = g_queue.front();
  g_queue.pop_front();
  uint8_t* out = base + keystroke;
  StoreBE16(out, event.virtual_key);
  StoreBE16(out + 2, event.unicode);
  StoreBE16(out + 4, kKeystrokeKeyDown);
  out[6] = user_index;
  out[7] = 0;
  return true;
}

}  // namespace nhl3on3::chat_keyboard
