// Xbox 360 STFS package extraction; see nhl3on3_stfs.h.

#include "nhl3on3_stfs.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace nhl3on3 {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kBlockSize = 0x1000;
constexpr uint32_t kEndOfChain = 0xFFFFFF;
constexpr uint64_t kMetadataOffset = 0x344;
constexpr uint64_t kVolumeDescriptorOffset = kMetadataOffset + 0x35;

uint32_t BE16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
uint32_t BE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
uint32_t LE16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t LE24(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16); }

struct Entry {
  std::string path;
  bool is_dir;
  uint32_t start_block;
  uint32_t length;
};

class Package {
 public:
  bool Open(const fs::path& path) {
    file_.open(path, std::ios::binary);
    uint8_t head[kVolumeDescriptorOffset + 0x40];
    if (!Read(0, head, sizeof(head))) return false;
    if (std::memcmp(head, "CON ", 4) && std::memcmp(head, "LIVE", 4) &&
        std::memcmp(head, "PIRS", 4)) {
      return false;
    }
    if (BE32(head + kMetadataOffset + 0x65) != 0) return false;  // not an STFS volume
    header_size_ = BE32(head + 0x340);
    blocks_per_hash_table_ = (head[kVolumeDescriptorOffset + 2] & 1) ? 1 : 2;
    table_block_count_ = LE16(head + kVolumeDescriptorOffset + 3);
    table_block_ = LE24(head + kVolumeDescriptorOffset + 5);
    return true;
  }

  std::optional<std::vector<Entry>> List() {
    std::vector<Entry> entries;
    uint32_t block = table_block_;
    for (uint32_t t = 0; t < table_block_count_; ++t) {
      uint8_t table[kBlockSize];
      if (!Read(BlockOffset(block), table, sizeof(table))) return std::nullopt;
      for (int i = 0; i < 0x40; ++i) {
        const uint8_t* e = table + i * 0x40;
        if (e[0] == 0) break;
        const uint32_t flags = e[40];
        const std::string name(reinterpret_cast<const char*>(e), flags & 0x3F);
        const uint32_t parent = BE16(e + 50);
        if (name.empty() || name == "." || name == ".." ||
            name.find_first_of("/\\:") != std::string::npos ||
            (parent != 0xFFFF && (parent >= entries.size() || !entries[parent].is_dir))) {
          return std::nullopt;
        }
        entries.push_back({parent == 0xFFFF ? name : entries[parent].path + "/" + name,
                           (flags & 0x80) != 0, LE24(e + 47), BE32(e + 52)});
      }
      const auto next = NextBlock(block);
      if (!next || *next == kEndOfChain) break;
      block = *next;
    }
    return entries;
  }

  // Copies the file's block chain to out.
  bool Extract(const Entry& entry, std::ofstream& out) {
    std::vector<char> buffer(kBlockSize);
    uint32_t block = entry.start_block;
    uint32_t remaining = entry.length;
    while (remaining) {
      if (block == kEndOfChain) return false;
      const uint32_t n = std::min(kBlockSize, remaining);
      if (!Read(BlockOffset(block), buffer.data(), n)) return false;
      out.write(buffer.data(), n);
      remaining -= n;
      const auto next = NextBlock(block);
      if (!next) return false;
      block = *next;
    }
    return bool(out);
  }

 private:
  bool Read(uint64_t offset, void* out, size_t size) {
    file_.clear();
    file_.seekg(std::streamoff(offset));
    file_.read(static_cast<char*>(out), std::streamsize(size));
    return file_.gcount() == std::streamsize(size);
  }

  uint64_t DataStart() const { return (uint64_t(header_size_) + kBlockSize - 1) & ~uint64_t(kBlockSize - 1); }

  uint64_t BlockOffset(uint32_t index) const {
    uint64_t block = index;
    uint64_t base = 170;
    for (int level = 0; level < 3; ++level) {
      block += ((index + base) / base) * blocks_per_hash_table_;
      if (index < base) break;
      base *= 170;
    }
    return DataStart() + (block << 12);
  }

  uint64_t HashBlockNumber(uint32_t index) const {
    if (index < 170) return 0;
    uint64_t block = (index / 170) * uint64_t(170 + blocks_per_hash_table_);
    block += (index / 28900 + 1) * uint64_t(blocks_per_hash_table_);
    return index < 28900 ? block : block + blocks_per_hash_table_;
  }

  std::optional<uint32_t> NextBlock(uint32_t index) {
    uint8_t entry[4];
    const uint64_t offset = DataStart() + (HashBlockNumber(index) << 12) + (index % 170) * 0x18 + 0x14;
    if (!Read(offset, entry, sizeof(entry))) return std::nullopt;
    return BE32(entry) & 0xFFFFFF;
  }

  std::ifstream file_;
  uint32_t header_size_ = 0;
  uint32_t blocks_per_hash_table_ = 2;
  uint32_t table_block_count_ = 0;
  uint32_t table_block_ = 0;
};

bool ExtractAll(Package& package, const std::vector<Entry>& entries, const fs::path& game_dir) {
  std::error_code ec;
  std::vector<const Entry*> files;
  for (const Entry& e : entries) {
    if (e.is_dir) {
      fs::create_directories(game_dir / e.path, ec);
    } else {
      files.push_back(&e);
    }
  }
  std::stable_partition(files.begin(), files.end(),
                        [](const Entry* e) { return e->path != "default.xex"; });
  for (const Entry* e : files) {
    const fs::path target = game_dir / e->path;
    fs::path part = target;
    part += ".part";
    fs::create_directories(target.parent_path(), ec);
    bool ok;
    {
      std::ofstream out(part, std::ios::binary | std::ios::trunc);
      ok = out && package.Extract(*e, out);
    }
    if (ok) {
      fs::rename(part, target, ec);
      ok = !ec;
    }
    if (!ok) {
      REXLOG_ERROR("[game] could not extract {} from the game package", e->path);
      fs::remove(part, ec);
      return false;
    }
  }
  return true;
}

}  // namespace

bool ExtractGamePackage(const fs::path& game_dir) {
  std::error_code ec;
  if (!fs::is_directory(game_dir, ec)) return false;
  for (const auto& item : fs::directory_iterator(game_dir, ec)) {
    if (!item.is_regular_file(ec) || item.file_size(ec) < 0x10000) continue;
    Package package;
    if (!package.Open(item.path())) continue;
    const auto entries = package.List();
    if (!entries || std::none_of(entries->begin(), entries->end(), [](const Entry& e) {
          return !e.is_dir && e.path == "default.xex";
        })) {
      continue;
    }
    REXLOG_INFO("[game] extracting game package {} into {}", item.path().filename().string(),
                game_dir.string());
    const auto start = std::chrono::steady_clock::now();
    if (!ExtractAll(package, *entries, game_dir)) return false;
    // The extracted data0.big / data0.bh are originals: let the front-end mod
    // start over from them.
    for (const char* stale : {"data0.nhl3on3", "data0.big.orig", "data0.bh.orig"}) {
      fs::remove(game_dir / stale, ec);
    }
    REXLOG_INFO("[game] extracted {} entries in {} ms", entries->size(),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
    return true;
  }
  return false;
}

void InstallTextFont(const fs::path& game_dir) {
  std::error_code ec;
  const fs::path target = game_dir / "fe" / "ion" / "fonts" / "arial.ttf";
  if (game_dir.empty() || !fs::is_directory(game_dir / "fe", ec) ||
      fs::is_regular_file(target, ec)) {
    return;
  }
  std::vector<fs::path> sources;
#ifdef _WIN32
  if (const char* windir = std::getenv("WINDIR")) {
    sources.push_back(fs::path(windir) / "Fonts" / "arial.ttf");
  }
  sources.push_back("C:\\Windows\\Fonts\\arial.ttf");
#else
  for (const char* path : {"/usr/share/fonts/truetype/msttcorefonts/Arial.ttf",
                           "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                           "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
                           "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
                           "/usr/share/fonts/TTF/LiberationSans-Regular.ttf"}) {
    sources.push_back(path);
  }
#endif
  for (const fs::path& source : sources) {
    if (!fs::is_regular_file(source, ec)) continue;
    fs::create_directories(target.parent_path(), ec);
    if (fs::copy_file(source, target, fs::copy_options::skip_existing, ec)) {
      REXLOG_INFO("[game] copied {} to {}", source.string(), target.string());
      return;
    }
  }
  REXLOG_WARN("[game] no system Arial found; names and popups may be missing text");
}

}  // namespace nhl3on3
