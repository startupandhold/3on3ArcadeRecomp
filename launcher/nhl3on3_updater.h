#pragma once

#include <filesystem>
#include <string>

// Updates the launcher's folder (the game and the launcher) from the latest
// GitHub release, like nhl3on3_launcher.ps1 does on Windows. Release folders
// carry version.txt (scripts/package_release.sh); a folder without it (a
// build folder) never updates. The work runs on a thread and uses curl, tar
// and sha256sum, so the launcher links nothing new.
namespace updater {

enum class State {
  kOff,          // no version.txt
  kChecking,     // asking GitHub for the latest release
  kCurrent,      // up to date, or the check failed (offline: say nothing)
  kAvailable,    // a newer release can be installed
  kDownloading,
  kInstalling,
  kFailed,       // the update failed (message says why); it can be retried
  kRestart,      // installed: start the new launcher
};

struct Status {
  State state = State::kOff;
  std::string version;   // installed release tag
  std::string latest;    // latest release tag
  std::string message;   // why the update failed
  float progress = -1;   // download progress 0..1, or -1 if unknown
};

// Reads dir/version.txt and starts the check.
void Start(const std::filesystem::path& dir);
Status Get();
// Downloads and installs the latest release (kAvailable or kFailed only).
void Install();
// Stops any download and waits for the thread.
void Shutdown();

}  // namespace updater
