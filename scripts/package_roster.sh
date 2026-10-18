#!/usr/bin/env bash
# Builds the optional roster pack users unzip next to the game:
#
#   out/release/NHL3on3-roster.zip
#     roster/version.txt
#     roster/roster.json
#     roster/headshots/*.png
#     roster/README.txt
#
# Usage: scripts/package_roster.sh [--roster PATH] [--headshots DIR]
#                                  [--version PATH] [--out PATH]
#
# Defaults look next to the repo for roster/roster.json and roster/headshots/
# (or game/roster/headshots/). Neither is committed; this zip is the download.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD

roster=$root/roster/roster.json
[[ -f $roster ]] || roster=$root/game/roster/roster.json
headshots=$root/roster/headshots
[[ -d $headshots ]] || headshots=$root/game/roster/headshots
version=$root/release/roster/version.txt
out=$root/out/release/NHL3on3-roster.zip

while [[ $# -gt 0 ]]; do
  case $1 in
    --roster) roster=$2; shift 2 ;;
    --headshots) headshots=$2; shift 2 ;;
    --version) version=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done

if [[ ! -f $roster ]]; then
  echo "missing roster.json (tried roster/ and game/roster/)" >&2
  exit 1
fi
if [[ ! -d $headshots ]]; then
  echo "missing headshots folder (tried roster/headshots and game/roster/headshots)" >&2
  exit 1
fi
if [[ ! -f $version ]]; then
  echo "missing version.txt ($version)" >&2
  exit 1
fi
shopt -s nullglob
pngs=("$headshots"/*.png)
if [[ ${#pngs[@]} -eq 0 ]]; then
  echo "no PNG files in $headshots" >&2
  exit 1
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/roster/headshots"
cp "$roster" "$stage/roster/roster.json"
cp "$version" "$stage/roster/version.txt"
cp release/roster/README.txt "$stage/roster/README.txt"
cp "${pngs[@]}" "$stage/roster/headshots/"

mkdir -p "$(dirname "$out")"
rm -f "$out"
(cd "$stage" && zip -qr -X "$out" roster)
echo "wrote $out (v$(tr -d '[:space:]' <"$version"), $(basename "$roster"), ${#pngs[@]} headshots)"
