// Keeps the game's options (camera, controls, volumes, rules, ...) between
// sessions. The game only holds them in memory: its save system never runs,
// and the options it would have saved are lost at exit.
//
// Every option goes through the front end's settings adapter
// (FE::CSettingsIonAdapter): sub_82580628(adapter, id) reads setting `id` and
// sub_82580C90(adapter, id, value) changes it, routing each id to the system
// that owns it. Changes made in the options menus are written to the
// game_settings line of nhl3on3.toml as "id=value" pairs and replayed through
// the same setter once the adapter exists. In online games they go back to
// the game's defaults (see UseDefaultGameSettings).

#include "nhl3on3_settings.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_STRING(game_settings, "", "NHL3on3/General",
                      "In-game options kept between sessions, as setting id=value pairs "
                      "separated by ','. Written by the game when an option is changed in "
                      "its menus; empty the line to go back to the game's defaults");

REXCVAR_DEFINE_BOOL(restore_game_settings, true, "NHL3on3/General",
                    "Apply game_settings at startup and record option changes into it");

namespace {

constexpr std::string_view kKey = "game_settings";
// Return address of the setter call in the adapter's "field changed" handler
// (sub_8258B668), i.e. a change the player made in a menu.
constexpr uint32_t kMenuChange = 0x8258B724;
constexpr uint32_t kSetterStart = 0x82580C90, kSetterEnd = 0x825815E0;
constexpr uint32_t kMaxSettingId = 188;

std::mutex g_mutex;
std::filesystem::path g_config_path;
std::map<uint32_t, int32_t> g_saved;
// The game's own value of each setting game_settings or a menu changed, read
// before the first change.
std::map<uint32_t, int32_t> g_defaults;
bool g_loaded = false;
std::atomic<bool> g_restored{false};
std::atomic<bool> g_want_defaults{false};
std::atomic<bool> g_using_defaults{false};

std::map<uint32_t, int32_t> Parse(std::string_view text) {
  std::map<uint32_t, int32_t> out;
  while (!text.empty()) {
    const size_t comma = text.find(',');
    std::string_view pair = text.substr(0, comma);
    text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    const size_t eq = pair.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    uint32_t id = 0;
    int32_t value = 0;
    const auto key = pair.substr(0, eq), val = pair.substr(eq + 1);
    if (std::from_chars(key.data(), key.data() + key.size(), id).ec == std::errc() &&
        std::from_chars(val.data(), val.data() + val.size(), value).ec == std::errc() &&
        id <= kMaxSettingId) {
      out[id] = value;
    }
  }
  return out;
}

std::string Format(const std::map<uint32_t, int32_t>& settings) {
  std::string out;
  for (const auto& [id, value] : settings) {
    if (!out.empty()) {
      out += ',';
    }
    out += fmt::format("{}={}", id, value);
  }
  return out;
}

// Caller holds g_mutex.
void LoadOnce() {
  if (!g_loaded) {
    g_loaded = true;
    g_saved = Parse(REXCVAR_GET(game_settings));
  }
}

// Replaces (or appends) the game_settings line, leaving the rest of the file
// as the launcher and the player wrote it. Caller holds g_mutex.
void WriteConfig(const std::string& value) {
  if (g_config_path.empty()) {
    return;
  }
  std::vector<std::string> lines;
  bool crlf = false, replaced = false;
  const std::string line = fmt::format("{} = \"{}\"", kKey, value);
  if (std::ifstream in(g_config_path, std::ios::binary); in) {
    for (std::string l; std::getline(in, l);) {
      crlf = crlf || (!l.empty() && l.back() == '\r');
      const size_t start = l.find_first_not_of(" \t");
      std::string_view rest =
          start == std::string::npos ? std::string_view{} : std::string_view(l).substr(start);
      if (rest.starts_with(kKey)) {
        rest.remove_prefix(kKey.size());
        const size_t eq = rest.find_first_not_of(" \t");
        if (eq != std::string_view::npos && rest[eq] == '=') {
          if (!replaced) {
            lines.push_back(line);
            replaced = true;
          }
          continue;
        }
      }
      if (!l.empty() && l.back() == '\r') {
        l.pop_back();
      }
      lines.push_back(std::move(l));
    }
  }
  if (!replaced) {
    while (!lines.empty() && lines.back().empty()) {
      lines.pop_back();
    }
    lines.push_back(line);
  }
  const auto temp = std::filesystem::path(g_config_path).concat(".tmp");
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      REXLOG_WARN("[settings] could not write {}", temp.string());
      return;
    }
    for (const auto& l : lines) {
      out << l << (crlf ? "\r\n" : "\n");
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, g_config_path, ec);
  if (ec) {
    REXLOG_WARN("[settings] could not replace {}: {}", g_config_path.string(), ec.message());
  }
}

void Remember(uint32_t id, int32_t value) {
  std::lock_guard lock(g_mutex);
  LoadOnce();
  auto [it, inserted] = g_saved.try_emplace(id, value);
  if (!inserted && it->second == value) {
    return;
  }
  it->second = value;
  const std::string text = Format(g_saved);
  rex::cvar::SetFlagByName(kKey, text);
  WriteConfig(text);
}

}  // namespace

namespace nhl3on3 {

void SetConfigPath(const std::filesystem::path& path) {
  std::lock_guard lock(g_mutex);
  g_config_path = path;
}

void UseDefaultGameSettings(bool use_defaults) {
  g_want_defaults.store(use_defaults);
}

}  // namespace nhl3on3

extern "C" void __imp__sub_82580628(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82580C90(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8258B5D0(PPCContext& __restrict ctx, uint8_t* base);

namespace {

int32_t ReadSetting(PPCContext& ctx, uint8_t* base, uint32_t adapter, uint32_t id) {
  const PPCContext caller = ctx;
  ctx.r3.u64 = adapter;
  ctx.r4.u64 = id;
  __imp__sub_82580628(ctx, base);
  const int32_t value = ctx.r3.s32;
  ctx = caller;
  return value;
}

void WriteSetting(PPCContext& ctx, uint8_t* base, uint32_t adapter, uint32_t id, int32_t value) {
  const PPCContext caller = ctx;
  ctx.r3.u64 = adapter;
  ctx.r4.u64 = id;
  ctx.r5.s64 = value;
  __imp__sub_82580C90(ctx, base);
  ctx = caller;
}

void NoteDefault(PPCContext& ctx, uint8_t* base, uint32_t adapter, uint32_t id) {
  {
    std::lock_guard lock(g_mutex);
    if (g_defaults.contains(id)) {
      return;
    }
  }
  const int32_t value = ReadSetting(ctx, base, adapter, id);
  std::lock_guard lock(g_mutex);
  g_defaults.try_emplace(id, value);
}

}  // namespace

// Settings adapter setter(adapter, id, value).
extern "C" REX_FUNC(sub_82580C90) {
  const uint32_t id = ctx.r4.u32, lr = uint32_t(ctx.lr);
  const int32_t value = ctx.r5.s32;
  if (lr == kMenuChange && id <= kMaxSettingId && ctx.r3.u32) {
    NoteDefault(ctx, base, ctx.r3.u32, id);
  }
  __imp__sub_82580C90(ctx, base);
  if (lr >= kSetterStart && lr < kSetterEnd) {
    return;  // one setting changing others
  }
  if (lr == kMenuChange) {
    REXLOG_INFO("[settings] menu set {} = {}", id, value);
    if (REXCVAR_GET(restore_game_settings) && id <= kMaxSettingId) {
      Remember(id, value);
    }
    return;
  }
  static std::atomic<uint32_t> logged{0};
  if (logged++ < 200) {
    REXLOG_INFO("[settings] game set {} = {} from {:08X}", id, value, lr);
  }
}

// Settings adapter instance (created on first use, then fetched for every
// settings access). The saved options are applied through the setter the
// first time it exists, and swapped for or back from the game's defaults when
// UseDefaultGameSettings changes.
extern "C" REX_FUNC(sub_8258B5D0) {
  __imp__sub_8258B5D0(ctx, base);
  const uint32_t adapter = ctx.r3.u32;
  if (!adapter) {
    return;
  }
  if (!g_restored.load(std::memory_order_relaxed) && !g_restored.exchange(true)) {
    std::map<uint32_t, int32_t> saved;
    {
      std::lock_guard lock(g_mutex);
      LoadOnce();
      saved = g_saved;
    }
    REXLOG_INFO("[settings] adapter {:08X} created; restoring {} saved settings", adapter,
                REXCVAR_GET(restore_game_settings) ? saved.size() : 0);
    if (REXCVAR_GET(restore_game_settings)) {
      for (const auto& [id, value] : saved) {
        NoteDefault(ctx, base, adapter, id);
        WriteSetting(ctx, base, adapter, id, value);
        REXLOG_INFO("[settings] restored {} = {}", id, value);
      }
    }
  }
  const bool want_defaults = g_want_defaults.load(std::memory_order_relaxed);
  if (g_using_defaults.load(std::memory_order_relaxed) == want_defaults ||
      g_using_defaults.exchange(want_defaults) == want_defaults) {
    return;
  }
  std::map<uint32_t, int32_t> values;
  {
    std::lock_guard lock(g_mutex);
    if (want_defaults) {
      values = g_defaults;
    } else {
      for (const auto& [id, value] : g_saved) {
        if (g_defaults.contains(id)) {
          values.emplace(id, value);
        }
      }
    }
  }
  REXLOG_INFO("[settings] online game {}: {} settings back to {}",
              want_defaults ? "joined" : "left", values.size(),
              want_defaults ? "the game's defaults" : "their saved values");
  for (const auto& [id, value] : values) {
    WriteSetting(ctx, base, adapter, id, value);
    REXLOG_INFO("[settings]   {} = {}", id, value);
  }
}
