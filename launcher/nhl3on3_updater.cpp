// See nhl3on3_updater.h. The release's assets are
//   NHL3on3-<tag>-linux_64.tar.gz      one folder holding the whole install
//   NHL3on3-<tag>-SHA256SUMS.txt       sha256sum lines for the archives
// The archive is unpacked beside the install (.update/) and each file renamed
// over the installed one, so settings, logs and game files, which the
// archive doesn't have, stay. A rename leaves a running program's file (the
// launcher's own) intact. version.txt goes last, so an update that fails
// partway is offered again.

#include "nhl3on3_updater.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace updater {
namespace {

constexpr const char* kLatestUrl =
    "https://api.github.com/repos/startupandhold/3on3ArcadeRecomp/releases/latest";
constexpr const char* kArchiveSuffix = "-linux_64.tar.gz";
constexpr const char* kSumsSuffix = "-SHA256SUMS.txt";
constexpr const char* kUserAgent = "User-Agent: NHL3on3-launcher";

std::mutex g_mutex;  // guards everything below but the thread
Status g_status;
fs::path g_dir, g_download;
std::string g_archive_url, g_sums_url;
uint64_t g_archive_size = 0;
std::thread g_thread;
std::atomic<pid_t> g_child{0};
std::atomic<bool> g_quit{false};

void Set(State state, const std::string& message = {}) {
  std::lock_guard lock(g_mutex);
  g_status.state = state;
  g_status.message = message;
}

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string FileName(const std::string& url) { return url.substr(url.rfind('/') + 1); }

bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::char_traits<char>::length(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Runs a program from PATH; its output goes to *out when given. True if it
// exits with 0. Only async-signal-safe calls between fork and exec.
bool Run(const std::vector<std::string>& args, std::string* out = nullptr) {
  std::vector<char*> argv;
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  int fds[2] = {-1, -1};
  if (out && pipe2(fds, O_CLOEXEC) != 0) return false;
  const int null = open("/dev/null", O_RDWR | O_CLOEXEC);
  const pid_t pid = fork();
  if (pid == 0) {
    dup2(null, STDIN_FILENO);
    dup2(out ? fds[1] : null, STDOUT_FILENO);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  if (null >= 0) close(null);
  if (out) close(fds[1]);
  if (pid < 0) {
    if (out) close(fds[0]);
    return false;
  }
  g_child = pid;
  if (out) {
    char buffer[4096];
    for (;;) {
      const ssize_t n = read(fds[0], buffer, sizeof(buffer));
      if (n > 0) {
        out->append(buffer, size_t(n));
      } else if (n == 0 || errno != EINTR) {
        break;
      }
    }
    close(fds[0]);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  g_child = 0;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// The value of the first "key" at or after `from`: a string (unescaped) or
// a bare number. `at` gets the position after it; npos if there is none.
std::string JsonValue(const std::string& json, const std::string& key, size_t from,
                      size_t* at = nullptr) {
  const std::string quoted = "\"" + key + "\"";
  for (size_t p = json.find(quoted, from); p != std::string::npos;
       p = json.find(quoted, p + 1)) {
    size_t i = p + quoted.size();
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i >= json.size() || json[i] != ':') continue;
    ++i;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    std::string value;
    if (i < json.size() && json[i] == '"') {
      for (++i; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) ++i;
        value += json[i];
      }
    } else {
      for (; i < json.size() && std::isdigit(static_cast<unsigned char>(json[i])); ++i) {
        value += json[i];
      }
    }
    if (at) *at = i;
    return value;
  }
  if (at) *at = std::string::npos;
  return {};
}

// True when release tag `tag` is newer than `current` (numbers in order).
bool Newer(const std::string& tag, const std::string& current) {
  auto numbers = [](const std::string& s) {
    std::vector<uint64_t> out;
    const std::regex digits("\\d+");
    for (auto it = std::sregex_iterator(s.begin(), s.end(), digits); it != std::sregex_iterator();
         ++it) {
      out.push_back(std::stoull(it->str()));
    }
    return out;
  };
  const std::vector<uint64_t> a = numbers(tag), b = numbers(current);
  if (a.empty() || b.empty()) return tag != current;
  for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
    const uint64_t x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
    if (x != y) return x > y;
  }
  return false;
}

std::vector<std::string> Curl(std::vector<std::string> args) {
  args.insert(args.begin(), {"curl", "-fsSL", "--retry", "2", "-H", kUserAgent});
  return args;
}

void Check(std::string version) {
  std::string json;
  if (!Run(Curl({"--max-time", "15", kLatestUrl}), &json)) {
    Set(State::kCurrent);
    return;
  }
  const std::string tag = JsonValue(json, "tag_name", 0);
  std::string archive, sums;
  for (size_t at = 0; at != std::string::npos;) {
    const std::string url = JsonValue(json, "browser_download_url", at, &at);
    if (EndsWith(url, kArchiveSuffix)) archive = url;
    if (EndsWith(url, kSumsSuffix)) sums = url;
  }
  // The asset's size follows its name.
  uint64_t size = 0;
  for (size_t at = 0; !archive.empty() && at != std::string::npos;) {
    if (JsonValue(json, "name", at, &at) == FileName(archive)) {
      const std::string bytes = JsonValue(json, "size", at);
      size = bytes.empty() ? 0 : std::stoull(bytes);
      break;
    }
  }
  std::lock_guard lock(g_mutex);
  if (tag.empty() || archive.empty() || sums.empty() || !Newer(tag, version)) {
    g_status.state = State::kCurrent;
    return;
  }
  g_status.state = State::kAvailable;
  g_status.latest = tag;
  g_archive_url = archive;
  g_sums_url = sums;
  g_archive_size = size;
}

void Update() {
  std::string archive_url, sums_url;
  fs::path dir;
  {
    std::lock_guard lock(g_mutex);
    archive_url = g_archive_url;
    sums_url = g_sums_url;
    dir = g_dir;
  }
  const fs::path work = dir / ".update", files = work / "files";
  const fs::path archive = work / FileName(archive_url);
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(files, ec);
  auto fail = [&](const std::string& message) {
    fs::remove_all(work, ec);
    Set(State::kFailed, message);
  };
  {
    std::lock_guard lock(g_mutex);
    g_download = archive;
  }
  if (!Run(Curl({"-o", archive.string(), archive_url}))) {
    if (!g_quit) fail("couldn't download " + FileName(archive_url) + ".");
    return;
  }
  Set(State::kInstalling);

  std::string sums, sum, want;
  if (!Run(Curl({"--max-time", "30", sums_url}), &sums)) {
    return fail("couldn't download the checksums.");
  }
  std::istringstream lines(sums);
  for (std::string line; std::getline(lines, line);) {
    std::istringstream words(line);
    std::string hash, name;
    words >> hash >> name;
    if (!name.empty() && name[0] == '*') name.erase(0, 1);
    if (name == archive.filename().string()) want = Lower(hash);
  }
  if (want.empty()) return fail("the release has no checksum for " + archive.filename().string());
  if (!Run({"sha256sum", archive.string()}, &sum)) return fail("sha256sum didn't run.");
  if (Lower(sum.substr(0, 64)) != want) {
    return fail("the download is damaged (checksum mismatch). Try again.");
  }
  if (!Run({"tar", "-xzf", archive.string(), "-C", files.string()})) {
    return fail("couldn't unpack the download.");
  }

  fs::path root;
  for (const auto& entry : fs::directory_iterator(files, ec)) {
    if (entry.is_directory()) root = entry.path();
  }
  if (root.empty() || !fs::is_regular_file(root / "nhl3on3", ec)) {
    return fail("the download doesn't contain nhl3on3.");
  }
  std::vector<fs::path> paths;
  for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
    if (entry.is_regular_file()) paths.push_back(entry.path());
  }
  std::stable_partition(paths.begin(), paths.end(),
                        [](const fs::path& p) { return p.filename() != "version.txt"; });
  for (const fs::path& from : paths) {
    const fs::path rel = from.lexically_relative(root), to = dir / rel;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec) return fail("couldn't replace " + rel.string() + ": " + ec.message());
  }
  fs::remove_all(work, ec);
  Set(State::kRestart);
}

}  // namespace

void Start(const fs::path& dir) {
  std::ifstream f(dir / "version.txt");
  std::string version;
  std::getline(f, version);
  version = Trim(version);
  {
    std::lock_guard lock(g_mutex);
    g_dir = dir;
    g_status = {};
    g_status.version = version;
    if (version.empty()) return;
    g_status.state = State::kChecking;
  }
  g_thread = std::thread(Check, version);
}

Status Get() {
  std::lock_guard lock(g_mutex);
  Status status = g_status;
  if (status.state == State::kDownloading && g_archive_size > 0) {
    std::error_code ec;
    const uintmax_t bytes = fs::file_size(g_download, ec);
    if (!ec) status.progress = std::min(1.0f, float(double(bytes) / double(g_archive_size)));
  }
  return status;
}

void Install() {
  {
    std::lock_guard lock(g_mutex);
    if (g_status.state != State::kAvailable && g_status.state != State::kFailed) return;
    g_status.state = State::kDownloading;
    g_status.message.clear();
  }
  if (g_thread.joinable()) g_thread.join();
  g_thread = std::thread(Update);
}

void Shutdown() {
  g_quit = true;
  if (const pid_t pid = g_child; pid > 0) kill(pid, SIGTERM);
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace updater
