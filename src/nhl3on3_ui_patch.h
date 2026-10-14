#pragma once

#include <filesystem>

namespace nhl3on3 {

// Patches the front-end screens in game_dir's data0.big / data0.bh with the
// team play mod (tools/ui_mod.py, emitted as nhl3on3_ui_patches.inc), unless
// the marker file says this version is already applied. The originals are
// kept as data0.big.orig / data0.bh.orig and every version is rebuilt from
// them. Leaves the files alone if they are not the expected game version.
// Must run before the runtime opens the game files.
void ApplyUiPatches(const std::filesystem::path& game_dir);

}  // namespace nhl3on3
