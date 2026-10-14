// Front-end tweaks: intro movie skip and menu text overrides.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(skip_intro, true, "NHL3on3/General",
                    "Skip the EA Sports Freestyle intro movie at boot");

REXCVAR_DECLARE(int32_t, team_play);

REXCVAR_DEFINE_STRING(menu_renames, "XBOX LIVE=ONLINE MULTIPLAYER", "NHL3on3/General",
                      "Front-end text replacements, 'OLD=NEW' pairs separated by ';'. "
                      "Matches whole localized strings exactly (they are upper case)");

REXCVAR_DEFINE_BOOL(log_ui_text, false, "NHL3on3/General",
                    "Log every string the front end localizes (to find text for menu_renames)");

namespace {

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

// The LocalizeString natives keep their result in a string object at sp+80:
// +0 allocator, +4 char* (NUL-terminated UTF-8).
constexpr uint32_t kResultOffset = 80;
constexpr uint32_t kDataOffset = 4;

struct Renames {
  std::mutex mutex;
  std::string source;
  std::unordered_map<std::string, std::string> text;
  std::unordered_map<std::string, uint32_t> guest;  // replacement -> guest copy
};

Renames& GetRenames() {
  static Renames renames;
  return renames;
}

void Parse(Renames& r, const std::string& spec) {
  r.source = spec;
  r.text.clear();
  size_t start = 0;
  while (start <= spec.size()) {
    size_t end = spec.find(';', start);
    if (end == std::string::npos) end = spec.size();
    const std::string pair = spec.substr(start, end - start);
    const size_t eq = pair.find('=');
    if (eq != std::string::npos && eq > 0) {
      r.text[pair.substr(0, eq)] = pair.substr(eq + 1);
    }
    start = end + 1;
  }
  // With team play on, the unranked quick match is the team play queue (an
  // open game that others join until everyone is ready; ranked stays 1v1).
  if (REXCVAR_GET(team_play) >= 3) {
    r.text.emplace("UNRANKED VERSUS PLAY", "ONLINE TEAM PLAY");
  }
}

// Returns the guest address of the replacement for `text`, or 0.
uint32_t FindReplacement(rex::memory::Memory* memory, const std::string& text) {
  Renames& r = GetRenames();
  std::lock_guard lock(r.mutex);
  const std::string spec = REXCVAR_GET(menu_renames);
  if (spec != r.source) {
    Parse(r, spec);
  }
  const auto it = r.text.find(text);
  if (it == r.text.end()) {
    return 0;
  }
  auto& address = r.guest[it->second];
  if (!address) {
    const uint32_t size = uint32_t(it->second.size() + 1);
    address = memory->SystemHeapAlloc(size);
    if (address) {
      std::memcpy(memory->TranslateVirtual(address), it->second.c_str(), size);
    }
  }
  return address;
}

// Original data pointers swapped out, keyed by the native's stack pointer.
thread_local std::vector<std::pair<uint32_t, uint32_t>> t_swapped;

void AfterLookup(uint32_t sp) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) {
    return;
  }
  uint8_t* base = memory->virtual_membase();
  const uint32_t field = sp + kResultOffset + kDataOffset;
  const uint32_t data = Load32(base, field);
  if (!data) {
    return;
  }
  const std::string text(reinterpret_cast<const char*>(base + data));
  if (REXCVAR_GET(log_ui_text)) {
    REXLOG_INFO("[ui] localized '{}'", text);
  }
  if (const uint32_t replacement = FindReplacement(memory, text)) {
    t_swapped.emplace_back(sp, data);
    Store32(base, field, replacement);
    if (REXCVAR_GET(log_ui_text)) {
      REXLOG_INFO("[ui]   -> '{}'", reinterpret_cast<const char*>(base + replacement));
    }
  }
}

// The string has been copied into a script value; put the original buffer back
// before the destructor frees it.
void AfterConvert(uint32_t sp) {
  if (t_swapped.empty() || t_swapped.back().first != sp) {
    return;
  }
  auto* memory = REX_KERNEL_MEMORY();
  if (memory) {
    Store32(memory->virtual_membase(), sp + kResultOffset + kDataOffset,
            t_swapped.back().second);
  }
  t_swapped.pop_back();
}

}  // namespace

// sub_82A11638 polls a movie player's state (r3, returned by the player's
// vtable +88) and fires _MoviePlayingCallback on 2 and _MovieStopCallback on 4.
// The boot movie is the first one ever played; reporting it as "stopped" while
// it plays runs the front end's normal end-of-movie path. A stop issued within
// a few dozen ms of playback starting is lost by the native player, whose
// shutdown then never finishes and the boot flow hangs, so let it play briefly.
void Nhl3on3MovieStateHook(PPCRegister& r3) {
  using Clock = std::chrono::steady_clock;
  static std::atomic<bool> handled{false};
  static Clock::time_point playing_since{};
  if (r3.u32 != 2 || handled.load(std::memory_order_relaxed)) {
    return;
  }
  const auto now = Clock::now();
  if (playing_since == Clock::time_point{}) {
    playing_since = now;
  }
  if (REXCVAR_GET(skip_intro) && now - playing_since < std::chrono::milliseconds(250)) {
    return;
  }
  handled.store(true, std::memory_order_relaxed);
  if (REXCVAR_GET(skip_intro)) {
    REXLOG_INFO("Intro movie skipped");
    r3.u64 = 4;
  }
}

void Nhl3on3LocalizeLookupHook(PPCRegister& r1) {
  AfterLookup(r1.u32);
}

void Nhl3on3LocalizeConvertHook(PPCRegister& r1) {
  AfterConvert(r1.u32);
}
