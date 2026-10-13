// Front-end tweaks: intro movie skip and menu text overrides.

#include <atomic>
#include <cstdint>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(skip_intro, true, "NHL3on3/General",
                    "Skip the EA Sports Freestyle intro movie at boot");

REXCVAR_DEFINE_BOOL(log_ui_text, false, "NHL3on3/General",
                    "Log every string the front end localizes (for finding menu text)");

namespace {

uint8_t* GuestBase() {
  auto* memory = REX_KERNEL_MEMORY();
  return memory ? memory->virtual_membase() : nullptr;
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

// sub_82A11638 polls a movie player's state (r3, returned by the player's
// vtable +88) and fires _MoviePlayingCallback on 2 and _MovieStopCallback on 4.
// The boot movie is the first one ever played; reporting its first "playing"
// as "stopped" runs the front end's normal end-of-movie path.
void Nhl3on3MovieStateHook(PPCRegister& r3) {
  static std::atomic<bool> handled{false};
  if (r3.u32 != 2 || handled.load(std::memory_order_relaxed)) {
    return;
  }
  handled.store(true, std::memory_order_relaxed);
  if (REXCVAR_GET(skip_intro)) {
    REXLOG_INFO("Intro movie skipped");
    r3.u64 = 4;
  }
}

// Diagnostic: LocalizeString (sub_82AC3FC0) right after the lookup fills the
// string object at sp+80.
void Nhl3on3LocalizeHook(PPCRegister& r1) {
  uint8_t* base = GuestBase();
  if (!base || !REXCVAR_GET(log_ui_text)) {
    return;
  }
  const uint32_t obj = r1.u32 + 80;
  std::string words;
  for (uint32_t i = 0; i < 4; ++i) {
    words += fmt::format(" {:08X}", Load32(base, obj + i * 4));
  }
  std::string text;
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t p = Load32(base, obj + i * 4);
    if (p < 0x40000000 || p >= 0xF0000000) {
      continue;
    }
    std::string s;
    for (uint32_t j = 0; j < 96; ++j) {
      const uint8_t c = base[p + j];
      if (!c) break;
      s += (c >= 0x20 && c < 0x7F) ? char(c) : '.';
    }
    text += fmt::format(" [{}]='{}'", i, s);
  }
  REXLOG_INFO("[ui] localize obj{}{}", words, text);
}
