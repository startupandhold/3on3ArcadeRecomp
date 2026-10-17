#!/usr/bin/env bash
# Packages the Release builds into the archives attached to a GitHub release:
#
#   out/release/NHL3on3-<version>-windows_64_portable.zip
#   out/release/NHL3on3-<version>-linux_64.tar.gz
#
# Usage: scripts/package_release.sh <version> [--windows DIR] [--linux DIR] [--vcredist DIR]
#
#   --windows   win-amd64-release build folder (default: out/build/win-amd64-release,
#               or the same path under /mnt/c/dev/nhl-3-on-3 when building on the
#               Windows side of WSL)
#   --linux     linux-amd64-release build folder (default: out/build/linux-amd64-release)
#   --vcredist  folder with the Visual C++ runtime DLLs (default: the newest
#               Microsoft.VC14x.CRT folder of a Visual Studio install)
#
# A platform whose build folder doesn't exist is skipped with a warning.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD

version=${1:-}
if [[ -z $version || $version == -* ]]; then
  echo "usage: $0 <version> [--windows DIR] [--linux DIR] [--vcredist DIR]" >&2
  exit 1
fi
shift

win_build=out/build/win-amd64-release
[[ -d $win_build ]] || win_build=/mnt/c/dev/nhl-3-on-3/out/build/win-amd64-release
linux_build=out/build/linux-amd64-release
vcredist=$(ls -d /mnt/c/Program\ Files*/Microsoft\ Visual\ Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT 2>/dev/null | sort -V | tail -1 || true)
while [[ $# -gt 0 ]]; do
  case $1 in
    --windows) win_build=$2; shift 2 ;;
    --linux) linux_build=$2; shift 2 ;;
    --vcredist) vcredist=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done

out=$root/out/release
mkdir -p "$out"
stage_root=$(mktemp -d)
trap 'rm -rf "$stage_root"' EXIT

# Files both platforms ship: the license and the game/ folder.
stage_common() {
  local dir=$1
  mkdir -p "$dir/game"
  cp LICENSE "$dir/LICENSE.txt"
  cp release/game/PUT_GAME_FILES_HERE.txt "$dir/game/"
}

require() {
  local f
  for f in "$@"; do
    if [[ ! -f $f ]]; then
      echo "missing $f" >&2
      exit 1
    fi
  done
}

# Refuses a build older than the source it should contain.
require_fresh() {
  local newer
  newer=$(find src nhl3on3_functions.toml CMakeLists.txt -type f -newer "$1" | head -1)
  if [[ -n $newer ]]; then
    echo "$1 is older than $newer; rebuild it before packaging" >&2
    exit 1
  fi
}

if [[ -f $win_build/nhl3on3.exe ]]; then
  name=NHL3on3-$version-windows_64_portable
  dir=$stage_root/$name
  mkdir -p "$dir"
  require "$win_build/nhl3on3.exe" "$win_build/rexruntime.dll" "$win_build/rexgpu-xenos.dll"
  require_fresh "$win_build/nhl3on3.exe"
  cp "$win_build/nhl3on3.exe" "$win_build/rexruntime.dll" "$win_build/rexgpu-xenos.dll" "$dir/"
  cp launcher/nhl3on3_launcher.ps1 "launcher/NHL3on3 Launcher.bat" "$dir/"
  cp release/README-windows.txt "$dir/README.txt"
  if [[ -n $vcredist && -d $vcredist ]]; then
    for dll in msvcp140.dll msvcp140_atomic_wait.dll vcruntime140.dll vcruntime140_1.dll; do
      require "$vcredist/$dll"
      cp "$vcredist/$dll" "$dir/"
    done
  else
    echo "warning: Visual C++ runtime not found; the Windows zip will need the VC++ redistributable installed" >&2
  fi
  stage_common "$dir"
  rm -f "$out/$name.zip"
  (cd "$stage_root" && zip -qr -X "$out/$name.zip" "$name")
  echo "wrote out/release/$name.zip"
else
  echo "warning: no Windows build in $win_build; skipping the Windows zip" >&2
fi

if [[ -f $linux_build/nhl3on3 ]]; then
  name=NHL3on3-$version-linux_64
  dir=$stage_root/$name
  mkdir -p "$dir"
  require "$linux_build/nhl3on3" "$linux_build/nhl3on3-launcher" \
          "$linux_build/librexruntime.so" "$linux_build/librexgpu-xenos.so"
  require_fresh "$linux_build/nhl3on3"
  cp "$linux_build/nhl3on3" "$linux_build/nhl3on3-launcher" \
     "$linux_build/librexruntime.so" "$linux_build/librexgpu-xenos.so" "$dir/"
  if readelf -d "$dir/nhl3on3" "$dir/librexruntime.so" "$dir/librexgpu-xenos.so" |
      grep -q 'NEEDED.*libTracyClient'; then
    cp "$linux_build/libTracyClient.so" "$dir/"
  fi
  strip --strip-unneeded "$dir/nhl3on3" "$dir/nhl3on3-launcher"
  chmod 755 "$dir/nhl3on3" "$dir/nhl3on3-launcher"
  cp release/README-linux.txt "$dir/README.txt"
  stage_common "$dir"
  rm -f "$out/$name.tar.gz"
  tar -C "$stage_root" --owner=0 --group=0 -czf "$out/$name.tar.gz" "$name"
  echo "wrote out/release/$name.tar.gz"
else
  echo "warning: no Linux build in $linux_build; skipping the Linux archive" >&2
fi

shopt -s nullglob
(cd "$out" && sha256sum NHL3on3-"$version"-*.zip NHL3on3-"$version"-*.tar.gz \
  > "NHL3on3-$version-SHA256SUMS.txt")
echo "wrote out/release/NHL3on3-$version-SHA256SUMS.txt"
