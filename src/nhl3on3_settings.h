#pragma once

#include <filesystem>

namespace nhl3on3 {

// The config file game_settings is written back to when an option changes.
void SetConfigPath(const std::filesystem::path& path);

}  // namespace nhl3on3
