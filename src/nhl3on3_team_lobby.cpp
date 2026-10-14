// Team play lobby: side choice on the Select Sides screen.
//
// The patched screen (tools/ui_mod.py) talks to this file through the
// GetControllerLogin script native, which normally takes a controller index
// below 4. Requests at 98 and up are answered here instead:
//   GetControllerLogin(99, me * 16 + count)   tick: local index, player count
//   GetControllerLogin(100 + side, me)        the local player picks a side
//   GetControllerLogin(102)                   -> the local side if it changed
//                                                since the last ask, else "-1"
//   GetControllerLogin(110 + side)            -> names on that side, ", "
//   GetControllerLogin(120 + i)               -> side of player i, "0" / "1"
//   GetControllerLogin(1000 + i, name)        name of player i
// Indices are session order, the same on every console (m_aPlayers, and the
// order the roster step visits members in).
//
// Picks travel as session control messages, which the stock screens ignore:
//   TPS|i=side       a console's own pick, on change and every second
//   TPT|i=side;...   the host's whole table, every second
//   TPP|i=lock       a console's own position lock (Starting Lineups), sent
//                    when the screen stores its user record at match start
// The roster step waits for every player's lock (see PositionsReady), since
// each console only knows its own and they must agree on all of them.
// Both are read off the wire by the datagram logger (OnControlMessage), so
// every console sees its own and everyone else's.

#include "nhl3on3_team_lobby.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(int32_t, team_play);

// Script native helpers: argument (index) -> value, value -> number,
// argument (index) -> C string ("" if not a string), C string -> returned value.
extern "C" REX_FUNC(sub_82C03D10);
extern "C" REX_FUNC(sub_82C00868);
extern "C" REX_FUNC(sub_829BD130);
extern "C" REX_FUNC(sub_829CC260);

namespace nhl3on3::team_lobby {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kOnlineGlobal = 0x83211FCC;  // +84: the game setup adaptor
constexpr uint32_t kSendToPeersSlot = 28;       // adaptor virtual (adaptor, text)
constexpr uint32_t kMaxPlayers = 12;
constexpr uint32_t kMaxPerSide = 3;
constexpr auto kResend = std::chrono::seconds(1);
constexpr auto kLockResend = std::chrono::milliseconds(250);

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

struct Player {
  std::string name;
  int side = -1;  // -1: no pick
  int lock = -1;  // position lock index from the user record; -1: unknown
};

struct Lobby {
  std::mutex mutex;
  std::vector<Player> players;
  uint32_t count = 0;
  int local = -1;
  int shown_side = -1;  // last local side answered to request 102
  bool dirty = false;   // local pick not yet sent
  Clock::time_point last_send{};
  Clock::time_point last_lock_send{};
  std::string logged;
};
Lobby g_lobby;

uint32_t EffectiveSide(const Lobby& lobby, uint32_t index) {
  if (index < lobby.players.size() && lobby.players[index].side >= 0) {
    return uint32_t(lobby.players[index].side);
  }
  return index % 2;
}

void LogTable(Lobby& lobby) {
  std::string text;
  for (uint32_t i = 0; i < lobby.count && i < lobby.players.size(); ++i) {
    const Player& p = lobby.players[i];
    text += fmt::format("{}{}={}{}", text.empty() ? "" : ", ", p.name.empty() ? "?" : p.name,
                        EffectiveSide(lobby, i), p.side < 0 ? " (default)" : "");
  }
  if (text != lobby.logged) {
    lobby.logged = text;
    REXLOG_INFO("[team] sides: {}", text);
  }
}

Player* At(Lobby& lobby, uint32_t index) {
  if (index >= kMaxPlayers) {
    return nullptr;
  }
  if (lobby.players.size() <= index) {
    lobby.players.resize(index + 1);
  }
  return &lobby.players[index];
}

void SetSide(Lobby& lobby, uint32_t index, int side) {
  if (side >= -1 && side <= 1) {
    if (Player* p = At(lobby, index)) {
      p->side = side;
    }
  }
}

void SetLock(Lobby& lobby, uint32_t index, int lock) {
  if (lock >= 0 && lock < 16) {
    if (Player* p = At(lobby, index)) {
      if (p->lock != lock) {
        REXLOG_INFO("[team] player {} position lock {}", index, lock);
      }
      p->lock = lock;
    }
  }
}

int ParseInt(std::string_view text) {
  int value = 0;
  bool negative = false, any = false;
  for (const char c : text) {
    if (c == '-' && !any) {
      negative = true;
    } else if (c >= '0' && c <= '9') {
      value = value * 10 + (c - '0');
      any = true;
    } else {
      break;
    }
  }
  return any ? (negative ? -value : value) : -1;
}

// "i=value" entries separated by ';'; the local player's own entry is skipped.
void ApplyEntries(Lobby& lobby, std::string_view data, void (*set)(Lobby&, uint32_t, int)) {
  while (!data.empty()) {
    const size_t end = data.find(';');
    const std::string_view entry = data.substr(0, end);
    if (const size_t eq = entry.find('='); eq != std::string_view::npos) {
      const int index = ParseInt(entry.substr(0, eq));
      if (index >= 0 && index != lobby.local) {
        set(lobby, uint32_t(index), ParseInt(entry.substr(eq + 1)));
      }
    }
    data = end == std::string_view::npos ? std::string_view() : data.substr(end + 1);
  }
}

// Copies `text` into one of two guest buffers (0: script replies, 1: sends).
uint32_t GuestText(const std::string& text, uint32_t slot) {
  constexpr uint32_t kSize = 512;
  static uint32_t buffers[2] = {};
  uint32_t& buffer = buffers[slot];
  auto* memory = REX_KERNEL_MEMORY();
  if (!buffer) {
    buffer = memory->SystemHeapAlloc(kSize);
    if (!buffer) {
      return 0;
    }
  }
  const size_t n = std::min<size_t>(text.size(), kSize - 1);
  auto* p = reinterpret_cast<char*>(memory->TranslateVirtual(buffer));
  std::memcpy(p, text.data(), n);
  p[n] = 0;
  return buffer;
}

void SendToPeers(const PPCContext& caller, uint8_t* base, const std::string& text) {
  const uint32_t online = Load32(base, kOnlineGlobal);
  const uint32_t adaptor = online ? Load32(base, online + 84) : 0;
  if (!adaptor) {
    return;
  }
  PPCFunc* send =
      rex::runtime::ResolveIndirectFunction(Load32(base, Load32(base, adaptor) + kSendToPeersSlot));
  const uint32_t guest = GuestText(text, 1);
  if (!send || !guest) {
    return;
  }
  PPCContext ctx = caller;
  ctx.r3.u64 = adaptor;
  ctx.r4.u64 = guest;
  send(ctx, base);
}

// Sends the local pick (or, on the host, the whole table) when it changed or
// a second has passed. Called on the game thread; the lock is not held.
void MaybeSend(const PPCContext& ctx, uint8_t* base) {
  std::string message;
  {
    std::lock_guard lock(g_lobby.mutex);
    Lobby& lobby = g_lobby;
    const auto now = Clock::now();
    if (lobby.local < 0 || (!lobby.dirty && now - lobby.last_send < kResend)) {
      return;
    }
    lobby.dirty = false;
    lobby.last_send = now;
    if (lobby.local == 0) {
      for (uint32_t i = 0; i < lobby.count && i < lobby.players.size(); ++i) {
        if (lobby.players[i].side >= 0) {
          message += fmt::format("{}{}={}", message.empty() ? "TPT|" : ";", i,
                                 lobby.players[i].side);
        }
      }
    } else if (uint32_t(lobby.local) < lobby.players.size() &&
               lobby.players[lobby.local].side >= 0) {
      message = fmt::format("TPS|{}={}", lobby.local, lobby.players[lobby.local].side);
    }
  }
  if (!message.empty()) {
    SendToPeers(ctx, base, message);
  }
}

int32_t ArgNumber(const PPCContext& caller, uint8_t* base, uint32_t index) {
  PPCContext ctx = caller;
  ctx.r3.u64 = index;
  sub_82C03D10(ctx, base);
  sub_82C00868(ctx, base);
  return int32_t(ctx.r3.u32);
}

std::string ArgString(const PPCContext& caller, uint8_t* base, uint32_t index) {
  PPCContext ctx = caller;
  ctx.r3.u64 = index;
  sub_829BD130(ctx, base);
  std::string text;
  for (uint32_t p = ctx.r3.u32, i = 0; p && i < 256 && base[p + i]; ++i) {
    text += char(base[p + i]);
  }
  return text;
}

// Answers one request; returns the text the script gets back.
std::string Handle(const PPCContext& ctx, uint8_t* base, int32_t code) {
  if (code == 99) {
    const int32_t arg = ArgNumber(ctx, base, 1);
    {
      std::lock_guard lock(g_lobby.mutex);
      g_lobby.local = arg >= 0 ? arg / 16 : -1;
      g_lobby.count = arg >= 0 ? std::min<uint32_t>(arg % 16, kMaxPlayers) : 0;
      LogTable(g_lobby);
    }
    MaybeSend(ctx, base);
    return {};
  }
  if (code == 100 || code == 101) {
    const int32_t me = ArgNumber(ctx, base, 1);
    const int side = code - 100;
    {
      std::lock_guard lock(g_lobby.mutex);
      Lobby& lobby = g_lobby;
      if (REXCVAR_GET(team_play) < 3 || me < 0 || uint32_t(me) >= kMaxPlayers) {
        return {};
      }
      lobby.local = me;
      uint32_t others = 0;
      for (uint32_t i = 0; i < lobby.count; ++i) {
        others += i != uint32_t(me) && EffectiveSide(lobby, i) == uint32_t(side);
      }
      if (EffectiveSide(lobby, me) == uint32_t(side) || others >= kMaxPerSide) {
        return {};
      }
      SetSide(lobby, me, side);
      lobby.dirty = true;
      REXLOG_INFO("[team] picked side {}", side);
      LogTable(lobby);
    }
    MaybeSend(ctx, base);
    return {};
  }
  std::lock_guard lock(g_lobby.mutex);
  Lobby& lobby = g_lobby;
  if (code == 102) {
    if (lobby.local < 0) {
      return "-1";
    }
    const int side = int(EffectiveSide(lobby, lobby.local));
    if (side == lobby.shown_side) {
      return "-1";
    }
    lobby.shown_side = side;
    return side ? "1" : "0";
  }
  if (code == 110 || code == 111) {
    std::string names;
    for (uint32_t i = 0; i < lobby.count && i < lobby.players.size(); ++i) {
      if (EffectiveSide(lobby, i) == uint32_t(code - 110) && !lobby.players[i].name.empty()) {
        names += (names.empty() ? "" : ", ") + lobby.players[i].name;
      }
    }
    return names;
  }
  if (code >= 120 && code < 120 + int32_t(kMaxPlayers)) {
    return EffectiveSide(lobby, code - 120) ? "1" : "0";
  }
  if (code >= 1000 && code < 1000 + int32_t(kMaxPlayers)) {
    const uint32_t index = code - 1000;
    std::string name = ArgString(ctx, base, 1);
    if (name.empty() || name == "undefined") {
      return {};
    }
    if (lobby.players.size() <= index) {
      lobby.players.resize(index + 1);
    }
    Player& p = lobby.players[index];
    if (p.name != name) {
      REXLOG_INFO("[team] player {} is {}", index, name);
      p = {name, -1};
    }
    lobby.count = std::max(lobby.count, index + 1);
  }
  return {};
}

}  // namespace

uint32_t SideOf(uint32_t index) {
  std::lock_guard lock(g_lobby.mutex);
  return EffectiveSide(g_lobby, index);
}

uint32_t LineupOf(uint32_t index) {
  std::lock_guard lock(g_lobby.mutex);
  const uint32_t side = EffectiveSide(g_lobby, index);
  uint32_t rank = 0;
  for (uint32_t i = 0; i < index; ++i) {
    rank += EffectiveSide(g_lobby, i) == side;
  }
  return side + 2 * rank;
}

int LockOf(uint32_t index) {
  std::lock_guard lock(g_lobby.mutex);
  return index < g_lobby.players.size() ? g_lobby.players[index].lock : -1;
}

bool PositionsReady(const PPCContext& ctx, uint8_t* base) {
  std::string message;
  bool ready = true;
  {
    std::lock_guard lock(g_lobby.mutex);
    Lobby& lobby = g_lobby;
    for (uint32_t i = 0; i < lobby.count; ++i) {
      ready = ready && i < lobby.players.size() && lobby.players[i].lock >= 0;
    }
    const auto now = Clock::now();
    if (!ready && lobby.local >= 0 && uint32_t(lobby.local) < lobby.players.size() &&
        lobby.players[lobby.local].lock >= 0 && now - lobby.last_lock_send > kLockResend) {
      lobby.last_lock_send = now;
      message = fmt::format("TPP|{}={}", lobby.local, lobby.players[lobby.local].lock);
    }
  }
  if (!message.empty()) {
    SendToPeers(ctx, base, message);
  }
  return ready;
}

void ClearPositions() {
  std::lock_guard lock(g_lobby.mutex);
  for (Player& p : g_lobby.players) {
    p.lock = -1;
  }
}

void OnControlMessage(std::string_view text) {
  const size_t a = text.find('|');
  const size_t b = a == std::string_view::npos ? a : text.find('|', a + 1);
  if (b == std::string_view::npos) {
    return;
  }
  const std::string_view sender = text.substr(0, a), command = text.substr(a + 1, b - a - 1);
  const std::string_view data = text.substr(b + 1);
  if (command != "TPS" && command != "TPT" && command != "TPP") {
    return;
  }
  std::lock_guard lock(g_lobby.mutex);
  Lobby& lobby = g_lobby;
  if (command == "TPP") {
    ApplyEntries(lobby, data, SetLock);
    return;
  }
  if (command == "TPT" && !(lobby.players.size() > 0 && lobby.players[0].name == sender)) {
    return;
  }
  ApplyEntries(lobby, data, SetSide);
  LogTable(lobby);
}

}  // namespace nhl3on3::team_lobby

// GetControllerLogin(controller) script native; see the top of this file.
extern "C" REX_FUNC(__imp__sub_82AC9248);
extern "C" REX_FUNC(sub_82AC9248) {
  using namespace nhl3on3::team_lobby;
  const int32_t code = ArgNumber(ctx, base, 0);
  if (code < 98) {
    __imp__sub_82AC9248(ctx, base);
    return;
  }
  const uint32_t text = GuestText(Handle(ctx, base, code), 0);
  ctx.r3.u64 = text;
  sub_829CC260(ctx, base);
}

// StoreUserRecord(record) script native. Starting Lineups stores
// "core_Username=name|S=..|O=..|L=..|P=lock" for each local user when the
// match starts; the local player's lock is shared with the other consoles.
extern "C" REX_FUNC(__imp__sub_82AC92A0);
extern "C" REX_FUNC(sub_82AC92A0) {
  using namespace nhl3on3::team_lobby;
  const std::string record = ArgString(ctx, base, 0);
  std::string message;
  {
    std::lock_guard lock(g_lobby.mutex);
    Lobby& lobby = g_lobby;
    const size_t user = record.find("core_Username="), at = record.find("|P=");
    if (lobby.local >= 0 && uint32_t(lobby.local) < lobby.players.size() &&
        user != std::string::npos && at != std::string::npos) {
      const std::string name = record.substr(user + 14, record.find('|', user) - user - 14);
      if (name == lobby.players[lobby.local].name) {
        SetLock(lobby, lobby.local, ParseInt(std::string_view(record).substr(at + 3)));
        lobby.last_lock_send = Clock::now();
        message = fmt::format("TPP|{}={}", lobby.local, lobby.players[lobby.local].lock);
      }
    }
  }
  if (!message.empty()) {
    SendToPeers(ctx, base, message);
  }
  __imp__sub_82AC92A0(ctx, base);
}
