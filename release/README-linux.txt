NHL 3 on 3 Arcade - Linux (x86-64)
==================================

A native PC port of 3 on 3 NHL Arcade (Xbox Live Arcade), recompiled with
ReXGlue, with online play. Self-contained: nothing is installed, and settings
are kept in this folder.

1. Add the game. Copy your own copy's Xbox 360 package file (or the files
   already extracted from it) into the game/ folder; see
   game/PUT_GAME_FILES_HERE.txt. The package is unpacked automatically the
   first time you play.

2. Start ./nhl3on3-launcher. Pick your settings (display, graphics, online
   gamertag) and press Play. The settings are saved to nhl3on3.toml.
   You can also run ./nhl3on3 directly, which uses the saved settings.

Requirements: x86-64 CPU with AVX2 (Intel Haswell / AMD Ryzen or newer), a
2023-or-newer distribution (glibc 2.38, GCC 13 libstdc++: Ubuntu 24.04,
Debian 13, Fedora 39, Arch, or newer), a Vulkan driver (Mesa or the NVIDIA
driver), and X11 or Wayland. A controller is recommended.

Online play: tick "Online play" on the launcher's Online tab and set a
gamertag. The default server is already filled in; no port forwarding is
needed.

In game: F11 or Alt+Enter toggles fullscreen.

Folder contents:
  nhl3on3                      the game
  nhl3on3-launcher             settings launcher
  librexruntime.so, librexgpu-xenos.so
                               ReXGlue runtime and GPU plugin
  game/                        put the game here

Keep the archive's folder layout; unpack with: tar xzf <archive>.tar.gz
