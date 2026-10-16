# NHL 3 on 3 Arcade — PC port

A native Windows and Linux port of **3 on 3 NHL Arcade** (Xbox Live Arcade,
2009), statically recompiled from the Xbox 360 executable with
[ReXGlue](https://github.com/rexglue/rexglue-sdk). It runs without an emulator, at higher
resolutions and frame rates, with ultrawide (WIP) support and online play.

**No game content is included.** You need your own copy of the game: the
release only contains the recompiled engine, and you add the game files to it.

## Download

Get the latest build from the
[Releases page](https://github.com/startupandhold/3on3ArcadeRecomp/releases).
Each release has one archive per platform:

| File | For |
| --- | --- |
| `NHL3on3-<version>-windows_64_portable.zip` | 64-bit Windows 10 / 11 |
| `NHL3on3-<version>-linux_64.tar.gz` | 64-bit (x86-64) Linux |
| `NHL3on3-<version>-SHA256SUMS.txt` | Checksums of the two archives |

Both are portable: unpack anywhere, nothing gets installed, and settings stay
in that folder. Each archive unpacks to a single folder:

```text
NHL3on3-<version>-windows_64_portable/     NHL3on3-<version>-linux_64/
├── nhl3on3.exe                            ├── nhl3on3
├── NHL3on3 Launcher.bat                   ├── nhl3on3-launcher
├── nhl3on3_launcher.ps1                   ├── librexruntime.so
├── rexruntime.dll                         ├── librexgpu-xenos.so
├── rexgpu-xenos.dll                       ├── README.txt
├── msvcp140*.dll, vcruntime140*.dll       ├── LICENSE.txt
├── README.txt                             └── game/
├── LICENSE.txt                                └── PUT_GAME_FILES_HERE.txt
└── game/
    └── PUT_GAME_FILES_HERE.txt
```

`nhl3on3.toml` (your settings) and `logs/` are created beside the game the
first time you save settings or play.

### Requirements

- A 64-bit CPU with AVX2: Intel Haswell (2013) / AMD Ryzen or newer.
- **Windows:** Windows 10 or 11 and a GPU with Direct3D 12 support. The
  Visual C++ runtime is included.
- **Linux:** a 2023-or-newer distribution (glibc 2.38 and libstdc++ from
  GCC 13: Ubuntu 24.04, Debian 13, Fedora 39, Arch, or newer), a Vulkan
  driver (Mesa or the NVIDIA driver), and an X11 or Wayland desktop.
  Everything else is bundled.
- A controller is recommended (anything SDL supports: Xbox, PlayStation,
  Switch Pro, and more).

## Setup

1. **Unpack the archive** for your platform. On Linux, keep the file
   permissions: `tar xzf NHL3on3-<version>-linux_64.tar.gz`.
2. **Copy your game package** into the `game` folder; see
   [Getting the game files](#getting-the-game-files). It's unpacked
   automatically the first time you play.
3. **Start the launcher**: `NHL3on3 Launcher.bat` on Windows, `nhl3on3-launcher`
   on Linux. Choose your settings and press **Play**.

After the first run you can also start `nhl3on3.exe` / `nhl3on3` directly; it
uses the saved settings.

On Windows, SmartScreen may warn about an unrecognized app the first time.
Choose **More info**, then **Run anyway**. The game isn't code-signed, so
Windows may also show an "Open File - Security Warning" when you start
`nhl3on3.exe` directly; the launcher clears that for you, or right-click the
exe, choose **Properties**, and tick **Unblock**.

## Getting the game files

The game comes as an Xbox 360 STFS package: a file with a 40-character
hexadecimal name, stored on the console's drive under
`Content/0000000000000000/58410975/000D0000/` (`58410975` is the game's title
ID). Copy that file as-is into the `game` folder.

The first time the game starts, it finds the package and unpacks it into
`game/` (a few seconds), then applies a small menu patch to `data0.big` and
`data0.bh`. After that you can delete the package file. If you already have
the extracted files instead, put them in `game/` so that `default.xex` is
directly inside it; that works too.

For extracting by hand, `tools/extract_stfs.py` in this repository does the
same with Python 3: `python3 tools/extract_stfs.py <package file> game`.

## Settings

The launcher writes `nhl3on3.toml` beside the game. It only writes settings
that differ from the defaults and keeps any other lines you add by hand.

| Tab | Settings |
| --- | --- |
| Display | Fullscreen, window size, monitor, aspect ratio (16:9, 21:9, 32:9, auto or custom), frame rate cap (60 to 240 or unlocked), VSync |
| Graphics | Internal resolution (720p, 1440p, 2160p), anisotropic filtering, native 2x MSAA |
| Game | Skip intro movie, mute audio, controller backend (Windows), keyboard as controller |
| Online | Online play, gamertag, server, connect through server, advertised IP |

Anything else can go in **Extra arguments** (for example `--log_fps=true`),
which is passed to the game as-is.

In game, **F11** or **Alt+Enter** toggles fullscreen. **Quit Game** on the
title menu exits.

## Online play

Turn on **Online play** on the launcher's Online tab and set a gamertag (up
to 15 characters). The community server (`nhl3on3-turtle.fly.dev`) is the
default, and **Connect through server** relays game traffic through it, so no
port forwarding is needed. If a gamertag is already in use, the server adds
a number to the second one (`turtle` becomes `turtle2`).

Up to three skaters and a goalie per team; every player needs the same
release version. A team's fourth player plays goalie, or set `--play_goalie=true`
(Extra arguments) to ask for it.
To run your own server, see [server/README.md](server/README.md).

## Troubleshooting

- **The game closes right away or can't find its files:** check that the
  package file, or `default.xex` if you extracted it yourself, is directly
  inside `game/` and not in a subfolder. The log (see below) shows whether
  the package was found and unpacked.
- **Crash at startup on an older CPU:** the build needs AVX2 (see
  Requirements).
- **The Linux launcher shows a Vulkan error:** install your GPU's Vulkan
  driver (for example `mesa-vulkan-drivers` on Debian/Ubuntu). You can still
  edit `nhl3on3.toml` by hand and run `./nhl3on3`.
- **Anything else:** the game writes logs to `logs/` beside the executable;
  please attach the newest one to a bug report.

## Building from source

You need the game's `default.xex` (the code is recompiled from it), the
ReXGlue SDK 0.10.0, CMake 3.25+, Ninja and Clang 20. Windows builds also need
Visual Studio 2022 (for the MSVC libraries and the Windows SDK).

1. Extract the game into `game/extracted/` (so that
   `game/extracted/default.xex` exists).
2. Unpack the ReXGlue SDK into `sdk/win-amd64/win-amd64/` and/or
   `sdk/linux-amd64/linux-amd64/`.
3. Generate the recompiled code:
   `sdk/<platform>/<platform>/bin/rexglue codegen nhl3on3_manifest.toml`
4. Build:
   - Windows: `scripts\build_win_release.bat` (or `scripts\build_win.bat` for
     RelWithDebInfo).
   - Linux: `scripts/build_linux_release.sh` (set `LLVM_DIR` and `NINJA` if
     Clang 20, `ld.lld` and Ninja aren't on `PATH`).

Output goes to `out/build/<preset>/`. When run from there, the game finds
`game/extracted/` by searching the parent folders, so no copying is needed.

### Making a release

1. Build both Release configurations (above).
2. Run `scripts/package_release.sh <version>` (from Linux or WSL). It writes
   the two archives and the checksum file to `out/release/`. It bundles the
   Visual C++ runtime from the local Visual Studio install.
3. Tag the commit and attach the files:

   ```sh
   git tag -a v0.1.0 -m "v0.1.0" && git push origin v0.1.0
   gh release create v0.1.0 out/release/NHL3on3-v0.1.0-* --title "v0.1.0" --generate-notes
   ```

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Game-specific code: display, UI patches, online play, team lobby |
| `generated/` | ReXGlue build glue (`generated/default/` is produced by codegen and not committed) |
| `launcher/` | Settings launchers: PowerShell (Windows), SDL3 + Dear ImGui (Linux) |
| `server/` | Matchmaking and relay server (Python), deployable to Fly.io |
| `tools/` | Reverse engineering and packaging helpers (STFS extraction, UI archive tools) |
| `release/` | Readmes and placeholders copied into the release archives |
| `scripts/` | Build and packaging scripts |

## Credits and legal

The code in this repository is released under the [MIT License](LICENSE).
That covers this project's own code only, not the game it runs or the
third-party components below, which keep their own licenses.

- Recompiled with [ReXGlue](https://github.com/rexglue/rexglue-sdk).
- The Linux launcher uses [Dear ImGui](https://github.com/ocornut/imgui) (MIT,
  `launcher/thirdparty/imgui/LICENSE.txt`) and [SDL3](https://libsdl.org) (zlib).

This is an unofficial fan project. It is not affiliated with or endorsed by
Electronic Arts, the NHL, the NHLPA or Microsoft. 3 on 3 NHL Arcade and all
related names, logos and content belong to their respective owners. This
repository and its releases contain no game assets; you must own the game to
play.
