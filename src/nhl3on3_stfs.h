#pragma once

#include <filesystem>

namespace nhl3on3 {

// If game_dir holds the game's Xbox 360 package (an STFS CON/LIVE/PIRS file
// containing default.xex) instead of the extracted files, extracts it into
// game_dir and returns true. default.xex is written last, so an interrupted
// extraction is redone on the next launch. The package file is left in place.
// Port of tools/_stfs_reader.py.
bool ExtractGamePackage(const std::filesystem::path& game_dir);

}  // namespace nhl3on3
