// Receives controllers streamed by tools/remote_pad.py over UDP.
//
// Packet (24 bytes, little-endian):
//   "NPAD", u8 slot, u8[3] reserved, u32 sequence,
//   u16 buttons, u8 left_trigger, u8 right_trigger, i16 lx, ly, rx, ry
// using XINPUT_GAMEPAD button bits. A slot counts as connected while its
// packets keep arriving.

#include "nhl3on3_remote_pad.h"

#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(remote_pad_port, 0, "NHL3on3/Input",
                     "UDP port to accept controllers from tools/remote_pad.py (0 = off); "
                     "each one drives the player slot it names")
    .range(0, 65535)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace nhl3on3::remote_pad {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kSlots = 4;
constexpr size_t kPacketSize = 24;
constexpr auto kTimeout = std::chrono::seconds(1);
constexpr size_t kMaxQueuedKeystrokes = 64;
constexpr int16_t kThumbThreshold = 0x4000;
constexpr uint8_t kTriggerThreshold = 0x40;
constexpr uint16_t kKeystrokeKeyDown = 0x0001;
constexpr uint16_t kKeystrokeKeyUp = 0x0002;

struct Gamepad {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

struct Keystroke {
  uint16_t virtual_key;
  uint16_t flags;
};

struct Slot {
  bool seen = false;
  Clock::time_point last_packet;
  uint32_t packet_number = 0;
  Gamepad pad;
  uint32_t pressed = 0;  // bits of kKeys
  std::deque<Keystroke> keystrokes;
};

// VK_PAD_* for XINPUT_GAMEPAD button bits 0-15, then triggers and left stick
// directions, in the order the SDK reports them.
struct Key {
  uint16_t virtual_key;
  bool (*pressed)(const Gamepad&);
};

template <uint16_t kButton>
bool Button(const Gamepad& pad) {
  return (pad.buttons & kButton) != 0;
}

constexpr std::array<Key, 20> kKeys = {{
    {0x5810, Button<0x0001>},  // DPAD_UP
    {0x5811, Button<0x0002>},  // DPAD_DOWN
    {0x5812, Button<0x0004>},  // DPAD_LEFT
    {0x5813, Button<0x0008>},  // DPAD_RIGHT
    {0x5814, Button<0x0010>},  // START
    {0x5815, Button<0x0020>},  // BACK
    {0x5816, Button<0x0040>},  // LTHUMB_PRESS
    {0x5817, Button<0x0080>},  // RTHUMB_PRESS
    {0x5805, Button<0x0100>},  // LSHOULDER
    {0x5804, Button<0x0200>},  // RSHOULDER
    {0x5800, Button<0x1000>},  // A
    {0x5801, Button<0x2000>},  // B
    {0x5802, Button<0x4000>},  // X
    {0x5803, Button<0x8000>},  // Y
    {0x5806, [](const Gamepad& p) { return p.left_trigger >= kTriggerThreshold; }},
    {0x5807, [](const Gamepad& p) { return p.right_trigger >= kTriggerThreshold; }},
    {0x5820, [](const Gamepad& p) { return p.ly >= kThumbThreshold; }},   // LTHUMB_UP
    {0x5821, [](const Gamepad& p) { return p.ly <= -kThumbThreshold; }},  // LTHUMB_DOWN
    {0x5822, [](const Gamepad& p) { return p.lx >= kThumbThreshold; }},   // LTHUMB_RIGHT
    {0x5823, [](const Gamepad& p) { return p.lx <= -kThumbThreshold; }},  // LTHUMB_LEFT
}};

std::mutex g_mutex;
std::array<Slot, kSlots> g_slots;

uint16_t LoadLE16(const uint8_t* p) {
  return uint16_t(p[0] | (p[1] << 8));
}

void StoreBE16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// Queues up events before down events, as XInput does.
void QueueKeystrokes(Slot& slot, uint32_t pressed) {
  const uint32_t changed = slot.pressed ^ pressed;
  for (int pass = 0; pass < 2; ++pass) {
    const bool down = pass == 1;
    for (size_t i = 0; i < kKeys.size(); ++i) {
      const uint32_t bit = 1u << i;
      if (!(changed & bit) || ((pressed & bit) != 0) != down) {
        continue;
      }
      if (slot.keystrokes.size() >= kMaxQueuedKeystrokes) {
        slot.keystrokes.pop_front();
      }
      slot.keystrokes.push_back({kKeys[i].virtual_key, down ? kKeystrokeKeyDown : kKeystrokeKeyUp});
    }
  }
  slot.pressed = pressed;
}

void HandlePacket(const uint8_t* data, size_t length, const std::string& sender) {
  if (length < kPacketSize || std::memcmp(data, "NPAD", 4) != 0 || data[4] >= kSlots) {
    return;
  }
  const uint32_t index = data[4];
  const uint8_t* p = data + 12;
  Gamepad pad;
  pad.buttons = LoadLE16(p);
  pad.left_trigger = p[2];
  pad.right_trigger = p[3];
  pad.lx = int16_t(LoadLE16(p + 4));
  pad.ly = int16_t(LoadLE16(p + 6));
  pad.rx = int16_t(LoadLE16(p + 8));
  pad.ry = int16_t(LoadLE16(p + 10));

  uint32_t pressed = 0;
  for (size_t i = 0; i < kKeys.size(); ++i) {
    if (kKeys[i].pressed(pad)) {
      pressed |= 1u << i;
    }
  }

  bool connected = false;
  {
    std::lock_guard lock(g_mutex);
    Slot& slot = g_slots[index];
    const auto now = Clock::now();
    if (!slot.seen || now - slot.last_packet > kTimeout) {
      connected = true;
      slot.pressed = 0;
      slot.keystrokes.clear();
    }
    slot.seen = true;
    slot.last_packet = now;
    if (std::memcmp(&slot.pad, &pad, sizeof(pad)) != 0) {
      slot.pad = pad;
      ++slot.packet_number;
    }
    QueueKeystrokes(slot, pressed);
  }
  if (connected) {
    REXLOG_INFO("[pad] remote controller from {} now drives slot {}", sender, index);
  }
}

void Listen(uint16_t port) {
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  auto sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    REXLOG_ERROR("[pad] could not listen for remote controllers on UDP port {}", port);
    return;
  }
  REXLOG_INFO("[pad] listening for remote controllers on UDP port {}", port);
  uint8_t buffer[256];
  for (;;) {
    sockaddr_in from{};
#if defined(_WIN32)
    int from_len = sizeof(from);
#else
    socklen_t from_len = sizeof(from);
#endif
    const int n = recvfrom(sock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                           reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n <= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    HandlePacket(buffer, size_t(n), std::string(ip) + ":" + std::to_string(ntohs(from.sin_port)));
  }
}

void StartListener() {
  static std::once_flag once;
  std::call_once(once, [] {
    const int32_t port = REXCVAR_GET(remote_pad_port);
    if (port > 0) {
      std::thread(Listen, uint16_t(port)).detach();
    }
  });
}

// Requires g_mutex.
Slot* ActiveSlot(uint32_t user_index) {
  if (user_index >= kSlots) {
    return nullptr;
  }
  Slot& slot = g_slots[user_index];
  return slot.seen && Clock::now() - slot.last_packet <= kTimeout ? &slot : nullptr;
}

}  // namespace

bool Drives(uint32_t user_index) {
  StartListener();
  std::lock_guard lock(g_mutex);
  return ActiveSlot(user_index) != nullptr;
}

bool GetState(uint8_t* base, uint32_t user_index, uint32_t state) {
  StartListener();
  std::lock_guard lock(g_mutex);
  const Slot* slot = ActiveSlot(user_index);
  if (!slot) {
    return false;
  }
  if (state) {
    uint8_t* s = base + state;
    StoreBE32(s, slot->packet_number);
    StoreBE16(s + 4, slot->pad.buttons);
    s[6] = slot->pad.left_trigger;
    s[7] = slot->pad.right_trigger;
    StoreBE16(s + 8, uint16_t(slot->pad.lx));
    StoreBE16(s + 10, uint16_t(slot->pad.ly));
    StoreBE16(s + 12, uint16_t(slot->pad.rx));
    StoreBE16(s + 14, uint16_t(slot->pad.ry));
  }
  return true;
}

bool GetCapabilities(uint8_t* base, uint32_t user_index, uint32_t caps) {
  if (!Drives(user_index)) {
    return false;
  }
  if (caps) {
    constexpr uint8_t kDevTypeGamepad = 1;
    constexpr uint8_t kDevSubTypeGamepad = 1;
    uint8_t* c = base + caps;
    std::memset(c, 0, 20);
    c[0] = kDevTypeGamepad;
    c[1] = kDevSubTypeGamepad;
    StoreBE16(c + 4, 0xF3FF);  // every button
    c[6] = 0xFF;
    c[7] = 0xFF;
    for (int i = 0; i < 4; ++i) {
      StoreBE16(c + 8 + 2 * i, 0xFFC0);
    }
    StoreBE16(c + 16, 0xFFFF);
    StoreBE16(c + 18, 0xFFFF);
  }
  return true;
}

int32_t PopKeystroke(uint8_t* base, uint32_t user_index, bool any_user, uint32_t keystroke) {
  StartListener();
  std::lock_guard lock(g_mutex);
  for (uint32_t i = 0; i < kSlots; ++i) {
    if (!any_user && i != user_index) {
      continue;
    }
    Slot* slot = ActiveSlot(i);
    if (!slot || slot->keystrokes.empty()) {
      continue;
    }
    const Keystroke event = slot->keystrokes.front();
    slot->keystrokes.pop_front();
    if (keystroke) {
      uint8_t* k = base + keystroke;
      StoreBE16(k, event.virtual_key);
      StoreBE16(k + 2, 0);
      StoreBE16(k + 4, event.flags);
      k[6] = uint8_t(i);
      k[7] = 0;
    }
    return int32_t(i);
  }
  return -1;
}

}  // namespace nhl3on3::remote_pad
