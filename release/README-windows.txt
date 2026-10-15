NHL 3 on 3 Arcade - Windows (64-bit, portable)
==============================================

A native PC port of 3 on 3 NHL Arcade (Xbox Live Arcade), recompiled with
ReXGlue, with online play. Portable: nothing is installed, and settings are
kept in this folder.

1. Add the game. Copy your own copy's Xbox 360 package file (or the files
   already extracted from it) into the game\ folder; see
   game\PUT_GAME_FILES_HERE.txt. The package is unpacked automatically the
   first time you play.

2. Start "NHL3on3 Launcher.bat". Pick your settings (display, graphics,
   online gamertag) and press Play. The settings are saved to nhl3on3.toml.
   You can also run nhl3on3.exe directly, which uses the saved settings.

Requirements: 64-bit Windows 10 or 11, a CPU with AVX2 (Intel Haswell / AMD
Ryzen or newer), and a GPU with Direct3D 12 support. A controller is
recommended.

Online play: tick "Online play" on the launcher's Online tab and set a
gamertag. The default server is already filled in; no port forwarding is
needed.

In game: F11 or Alt+Enter toggles fullscreen.

Folder contents:
  nhl3on3.exe                  the game
  NHL3on3 Launcher.bat         settings launcher (runs nhl3on3_launcher.ps1)
  rexruntime.dll, rexgpu-xenos.dll
                               ReXGlue runtime and GPU plugin
  msvcp140*.dll, vcruntime140*.dll
                               Microsoft Visual C++ runtime
  game\                        put the game here

Windows SmartScreen may warn about an unrecognized app the first time; choose
"More info" and then "Run anyway". The game isn't code-signed: if Windows asks
"Are you sure you want to run this software?" when you start nhl3on3.exe
directly, start it through the launcher once (it clears this), or right-click
nhl3on3.exe, choose Properties and tick "Unblock".
