// Restores the game's own debug output.
//
// The release build keeps its logging calls but compiles the logger bodies to
// a bare `blr`. Guest functions are weak symbols, so defining them here
// receives every message. Call sites are inconsistent about which argument is
// the format string, so the first argument register pointing at a printable
// string inside the image is used as the format, with the following registers
// as its arguments.

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(guest_log, false, "NHL3on3/Debug",
                    "Log the game's built-in debug messages (online/login diagnostics)");

REXCVAR_DEFINE_STRING(guest_log_backtrace, "Disconnecting from Lobby", "NHL3on3/Debug",
                      "Log the guest call stack for game messages containing this text");

namespace {

constexpr uint32_t kImageBegin = 0x82000000;
constexpr uint32_t kImageEnd = 0x83260000;

bool InImage(uint32_t address) {
  return address >= kImageBegin && address < kImageEnd;
}

bool Readable(uint32_t address) {
  if (InImage(address)) {
    return true;
  }
  if (address < 0x10000) {
    return false;
  }
  auto* memory = REX_KERNEL_MEMORY();
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  uint32_t protect = 0;
  return heap && heap->QueryProtect(address, &protect) &&
         (protect & rex::memory::kMemoryProtectRead);
}

// Returns the string at a guest address if it looks like text. Format strings
// are literals, so `image_only` restricts the search to the image.
bool ReadText(const uint8_t* base, uint32_t address, std::string& out, size_t min_length,
              bool image_only) {
  out.clear();
  if (image_only ? !InImage(address) : !Readable(address)) {
    return false;
  }
  for (uint32_t a = address; out.size() < 400; ++a) {
    if ((a & 0xFFF) == 0 && a != address && !Readable(a)) {
      return false;
    }
    uint8_t c = base[a];
    if (!c) {
      break;
    }
    if ((c < 0x20 && c != '\n' && c != '\t' && c != '\r') || c >= 0x7F) {
      return false;
    }
    out.push_back(char(c));
  }
  return out.size() >= min_length;
}

std::string Format(const uint8_t* base, const std::string& fmt, const uint64_t* args,
                   size_t arg_count) {
  std::string out;
  size_t next = 0;
  auto take = [&]() -> uint64_t { return next < arg_count ? args[next++] : 0; };
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%') {
      out.push_back(fmt[i]);
      continue;
    }
    size_t start = i++;
    while (i < fmt.size() && std::string_view("-+ #0123456789.*").find(fmt[i]) != std::string_view::npos) {
      if (fmt[i] == '*') {
        take();
      }
      ++i;
    }
    bool wide = false;
    while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'I' ||
                              fmt[i] == '6' || fmt[i] == '4')) {
      wide |= fmt[i] == 'l' || fmt[i] == '6';
      ++i;
    }
    if (i >= fmt.size()) {
      break;
    }
    char spec[32];
    const char conv = fmt[i];
    switch (conv) {
      case '%':
        out.push_back('%');
        break;
      case 's': {
        uint32_t ptr = uint32_t(take());
        std::string s;
        out += ReadText(base, ptr, s, 0, false) ? s : fmt::format("<{:08X}>", ptr);
        break;
      }
      case 'c':
        out.push_back(char(take()));
        break;
      case 'd':
      case 'i':
        std::snprintf(spec, sizeof(spec), "%lld",
                      wide ? (long long)int64_t(take()) : (long long)int32_t(take()));
        out += spec;
        break;
      case 'u':
        std::snprintf(spec, sizeof(spec), "%llu",
                      wide ? (unsigned long long)take() : (unsigned long long)uint32_t(take()));
        out += spec;
        break;
      case 'x':
      case 'X':
      case 'p':
        std::snprintf(spec, sizeof(spec), conv == 'X' ? "%llX" : "%llx",
                      wide ? (unsigned long long)take() : (unsigned long long)uint32_t(take()));
        out += spec;
        break;
      default:
        take();
        out += fmt.substr(start, i - start + 1);
        break;
    }
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

uint32_t LoadGuest32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// The loggers are leaf functions, so the caller's return address is still in
// LR. Every other frame starts with the back chain, with the saved LR 8 bytes
// below the caller's stack pointer.
std::string Backtrace(const PPCContext& ctx, const uint8_t* base) {
  std::string out = fmt::format("{:08X}", uint32_t(ctx.lr));
  uint32_t sp = ctx.r1.u32;
  for (int depth = 0; depth < 16; ++depth) {
    if (!Readable(sp)) {
      break;
    }
    const uint32_t caller_sp = LoadGuest32(base, sp);
    if (caller_sp <= sp || !Readable(caller_sp - 8)) {
      break;
    }
    const uint32_t lr = LoadGuest32(base, caller_sp - 8);
    if (!InImage(lr)) {
      break;
    }
    out += fmt::format(" <- {:08X}", lr);
    sp = caller_sp;
  }
  return out;
}

void LogGuestMessage(const char* source, PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(guest_log)) {
    return;
  }
  const uint64_t regs[8] = {ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64,
                            ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  std::string fmt;
  size_t fmt_index = 0;
  for (; fmt_index < 4; ++fmt_index) {
    if (ReadText(base, uint32_t(regs[fmt_index]), fmt, 2, true)) {
      break;
    }
  }
  if (fmt_index == 4) {
    return;
  }
  std::string line = Format(base, fmt, regs + fmt_index + 1, 7 - fmt_index);
  // The game logger doubles as the allocator naming hook; drop allocator names.
  if (line.find(' ') == std::string::npos || line.starts_with("EASTL") ||
      line.starts_with("operator new")) {
    return;
  }

  static std::mutex mutex;
  static std::string last;
  static uint32_t repeats = 0;
  std::lock_guard lock(mutex);
  if (line == last) {
    if (++repeats % 500 == 0) {
      REXLOG_INFO("[{}] (repeated {} times) {}", source, repeats, line);
    }
    return;
  }
  last = line;
  repeats = 0;
  REXLOG_INFO("[{}] {}", source, line);

  const std::string trigger = REXCVAR_GET(guest_log_backtrace);
  if (!trigger.empty() && line.find(trigger) != std::string::npos) {
    REXLOG_INFO("[{}]   backtrace: {}", source, Backtrace(ctx, base));
  }
}

const uint8_t* GuestBase() {
  auto* memory = REX_KERNEL_MEMORY();
  return memory ? memory->virtual_membase() : nullptr;
}

std::string FourCC(uint32_t value) {
  std::string out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const char c = char(value >> shift);
    out.push_back(c >= 0x20 && c < 0x7F ? c : '.');
  }
  return out;
}

std::string LobbyBody(const uint8_t* base, uint32_t address) {
  std::string out;
  for (uint32_t a = address; Readable(a) && base[a] && out.size() < 160; ++a) {
    const char c = char(base[a]);
    out += c == '\n' ? std::string("\\n") : std::string(1, c >= 0x20 && c < 0x7F ? c : '.');
  }
  return out;
}

std::string LobbyPersona(const uint8_t* base, uint32_t lobby) {
  std::string persona;
  ReadText(base, lobby + 6132, persona, 0, false);
  return persona;
}

}  // namespace

std::string Nhl3on3GuestBacktrace(const PPCContext& ctx, const uint8_t* base) {
  return Backtrace(ctx, base);
}

void Nhl3on3LobbyReplyHook(PPCRegister& r1, PPCRegister& r31) {
  const uint8_t* base = GuestBase();
  if (!REXCVAR_GET(guest_log) || !base) {
    return;
  }
  const uint32_t sp = r1.u32;
  REXLOG_INFO("[lobby] reply {} status {:08X} state {} persona '{}' body '{}'",
              FourCC(LoadGuest32(base, sp + 84)), LoadGuest32(base, sp + 88),
              LoadGuest32(base, r31.u32 + 6904), LobbyPersona(base, r31.u32),
              LobbyBody(base, LoadGuest32(base, sp + 80)));
}

void Nhl3on3LobbyReplyClaimedHook(PPCRegister& r1, PPCRegister& r3) {
  const uint8_t* base = GuestBase();
  if (!REXCVAR_GET(guest_log) || !base || r3.u32 == 0) {
    return;
  }
  REXLOG_INFO("[lobby]   reply {} claimed by a pending request ({:X})",
              FourCC(LoadGuest32(base, r1.u32 + 84)), r3.u32);
}

void Nhl3on3LobbyPersonaHook(PPCRegister& r31) {
  const uint8_t* base = GuestBase();
  if (!REXCVAR_GET(guest_log) || !base) {
    return;
  }
  REXLOG_INFO("[lobby]   pers stored persona '{}'", LobbyPersona(base, r31.u32));
}

void Nhl3on3LobbyLognHook(PPCRegister& r31) {
  const uint8_t* base = GuestBase();
  if (!REXCVAR_GET(guest_log) || !base) {
    return;
  }
  static std::mutex mutex;
  static std::string last = "<unset>";
  const std::string persona = LobbyPersona(base, r31.u32);
  std::lock_guard lock(mutex);
  if (persona != last) {
    last = persona;
    REXLOG_INFO("[lobby] logn status sees persona '{}' (lobby {:08X}, state {})", persona,
                r31.u32, LoadGuest32(base, r31.u32 + 6904));
  }
}

extern "C" REX_FUNC(sub_82830A88) {
  LogGuestMessage("game", ctx, base);
}

extern "C" REX_FUNC(sub_82CC2940) {
  LogGuestMessage("dirtysdk", ctx, base);
}
