#pragma once

#include <filesystem>

namespace nhl3on3 {

// If game_dir holds the game's Xbox 360 package (an STFS CON/LIVE/PIRS file
// containing default.xex) instead of the extracted files, extracts it into
// game_dir and returns true. default.xex is written last, so an interrupted
// extraction is redone on the next launch. The package file is left in place.
// Port of tools/_stfs_reader.py.
bool ExtractGamePackage(const std::filesystem::path& game_dir);

// The front end draws runtime text (names, popups, the copyright line) with
// fe/ion/fonts/arial.ttf, which the XBLA package does not contain. Copies the
// system's Arial (or a metric-compatible substitute on Linux) there if missing.
void InstallTextFont(const std::filesystem::path& game_dir);

}  // namespace nhl3on3
