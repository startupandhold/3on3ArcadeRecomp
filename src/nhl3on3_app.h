// nhl3on3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>

#include "nhl3on3_display.h"
#include <rex/system/xmemory.h>

class Nhl3on3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Nhl3on3App>(new Nhl3on3App(ctx, "nhl3on3",
        PPCImageConfig));
  }

  // Settings come from nhl3on3.toml beside the executable (written by the
  // launcher) unless a config path was given.
  //
  // Without --game_data_root, look for game/ (installed layout) or
  // game/extracted/ (dev checkout) beside the executable or in any parent.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (paths.config_path.empty()) {
      paths.config_path = rex::filesystem::GetExecutableFolder() / "nhl3on3.toml";
    }
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    for (auto dir = rex::filesystem::GetExecutableFolder(); !dir.empty();
         dir = dir.parent_path()) {
      for (const auto& candidate : {dir / "game", dir / "game" / "extracted"}) {
        if (std::filesystem::is_regular_file(candidate / "default.xex", ec)) {
          paths.game_data_root = candidate;
          return;
        }
      }
      if (dir == dir.parent_path()) {
        break;
      }
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Full XBLA license instead of the trial (same as Xenia's license_mask=1).
    SetDefaultFlag("license_mask", "1");

    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    // GPU cvars are registered by the plugin, so load it here (the base class
    // skips loading when config.graphics is already set) and then adjust them.
    if (!config.graphics) {
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin);
    }
    // The game's UI font and HUD textures use fetch constants with the
    // "invalid" type bits; strict checking skips those draws, leaving menus,
    // scoreboard and pause screen without text.
    SetDefaultFlag("gpu_allow_invalid_fetch_constants", "true");
    // Dynamic (TrueType) text is stamped into the glyph cache on the GPU and
    // then read by the CPU, so resolves must reach guest memory before the
    // CPU continues; the delayed "fast"/"some" modes still lose glyphs.
    // Without it popups, the legal screen and the copyright line are blank or
    // scrambled (seen on D3D12).
    SetDefaultFlag("readback_resolve", "full");
    nhl3on3::ConfigureDisplay();
  }

  // NHL3ON3_DUMP_IMAGE=1: write the decrypted, decompressed guest image to
  // guest_image_dump.bin beside the executable and exit without booting.
  // Offline RE tools (tools/find_missing_indirect_targets.py) scan this file.
  void OnPostSetup() override {
    const char* dump = std::getenv("NHL3ON3_DUMP_IMAGE");
    if (!dump || !*dump) {
      return;
    }
    const auto* img = runtime()->memory()->TranslateVirtual<const uint8_t*>(
        static_cast<uint32_t>(PPCImageConfig.image_base));
    const auto out = rex::filesystem::GetExecutableFolder() / "guest_image_dump.bin";
    if (std::FILE* f = std::fopen(out.string().c_str(), "wb")) {
      std::fwrite(img, 1, PPCImageConfig.image_size, f);
      std::fclose(f);
      std::fprintf(stderr, "[img-dump] wrote %llu bytes -> %s\n",
                   static_cast<unsigned long long>(PPCImageConfig.image_size),
                   out.string().c_str());
    }
    std::_Exit(0);
  }

 private:
  // F11 or Alt+Enter toggles fullscreen; other keys go to the SDK's overlay
  // binds (F3, F4, Backtick, ...) as in rex::ReXApp::OnKeyDown.
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    const auto key = e.virtual_key();
    if (key == rex::ui::VirtualKey::kF11 ||
        (key == rex::ui::VirtualKey::kReturn && e.is_alt_pressed())) {
      if (!e.prev_state() && window()) {
        rex::cvar::SetFlagByName("fullscreen", window()->IsFullscreen() ? "false" : "true");
      }
      e.set_handled(true);
      return;
    }
    rex::ui::ProcessKeyEvent(e);
  }

  // Changes a cvar's default without overriding a value from the command line
  // or config file.
  static void SetDefaultFlag(std::string_view name, std::string_view value) {
    if (!rex::cvar::HasNonDefaultValue(name)) {
      rex::cvar::SetFlagByName(name, value);
    }
  }
};
