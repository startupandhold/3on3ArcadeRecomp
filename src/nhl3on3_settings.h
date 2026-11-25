#pragma once

#include <filesystem>

namespace nhl3on3 {

// The config file game_settings is written back to when an option changes.
void SetConfigPath(const std::filesystem::path& path);

// In an online game every console runs the same simulation, so options
// (CPU difficulty, rules, ...) must agree; nothing sends one console's to the
// others. While true, the options game_settings changed are back at the
// game's defaults, the same on every console; false restores the saved ones.
// Takes effect the next time the game uses its settings.
void UseDefaultGameSettings(bool use_defaults);

}  // namespace nhl3on3
