// Applies the front-end mod to data0.big at startup; see nhl3on3_ui_patch.h.
// The archive layout follows tools/ui_mod.py (rebuild): a patched screen is
// appended uncompressed and both directories point at the new copy.

#include "nhl3on3_ui_patch.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace nhl3on3 {
namespace {

struct Run {
  uint32_t offset;
  uint32_t size;
  const uint8_t* bytes;
};

struct Screen {
  const char* entry;
  uint32_t size;  // uncompressed, before patching
  uint32_t crc;   // CRC-32 of the uncompressed original
  const Run* runs;
  size_t run_count;
};

#include "nhl3on3_ui_patches.inc"

constexpr const char* kMarker = "data0.nhl3on3";

using Bytes = std::vector<uint8_t>;

uint32_t Crc32(const Bytes& data) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (const uint8_t b : data) {
    c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  }
  return ~c;
}

uint32_t LoadBE32(const Bytes& d, size_t at) {
  return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) |
         d[at + 3];
}

void StoreBE32(Bytes& d, size_t at, uint32_t v) {
  d[at] = uint8_t(v >> 24);
  d[at + 1] = uint8_t(v >> 16);
  d[at + 2] = uint8_t(v >> 8);
  d[at + 3] = uint8_t(v);
}

// EA RefPack (header 0x10FB / 0x11FB / 0x90FB / 0x91FB); other data is
// returned as is. nullopt if the stream is malformed.
std::optional<Bytes> Unpack(const Bytes& in) {
  if (in.size() < 2 || in[1] != 0xFB || ((in[0] & 0xFE) != 0x10 && (in[0] & 0xFE) != 0x90)) {
    return in;
  }
  const size_t size_len = (in[0] & 0x80) ? 4 : 3;
  size_t p = 2 + ((in[0] & 0x01) ? size_len : 0);
  if (p + size_len > in.size()) {
    return std::nullopt;
  }
  size_t out_size = 0;
  for (size_t i = 0; i < size_len; ++i) {
    out_size = (out_size << 8) | in[p + i];
  }
  p += size_len;
  Bytes out;
  out.reserve(out_size);
  auto literal = [&](size_t n) {
    if (p + n > in.size()) {
      return false;
    }
    out.insert(out.end(), in.begin() + p, in.begin() + p + n);
    p += n;
    return true;
  };
  while (p < in.size()) {
    const uint8_t b0 = in[p];
    size_t length = 0, offset = 0;
    if (b0 < 0xC0 && p + (b0 < 0x80 ? 2 : 3) > in.size()) {
      return std::nullopt;
    }
    if (b0 < 0x80) {
      const uint8_t b1 = in[p + 1];
      p += 2;
      if (!literal(b0 & 3)) return std::nullopt;
      length = ((b0 >> 2) & 7) + 3;
      offset = ((b0 & 0x60) << 3) + b1 + 1;
    } else if (b0 < 0xC0) {
      const uint8_t b1 = in[p + 1], b2 = in[p + 2];
      p += 3;
      if (!literal(b1 >> 6)) return std::nullopt;
      length = (b0 & 0x3F) + 4;
      offset = ((b1 & 0x3F) << 8) + b2 + 1;
    } else if (b0 < 0xE0) {
      if (p + 4 > in.size()) return std::nullopt;
      const uint8_t b1 = in[p + 1], b2 = in[p + 2], b3 = in[p + 3];
      p += 4;
      if (!literal(b0 & 3)) return std::nullopt;
      length = ((b0 & 0x0C) << 6) + b3 + 5;
      offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
    } else if (b0 < 0xFC) {
      p += 1;
      if (!literal(((b0 & 0x1F) << 2) + 4)) return std::nullopt;
      continue;
    } else {
      p += 1;
      if (!literal(b0 & 3)) return std::nullopt;
      break;
    }
    if (offset > out.size()) {
      return std::nullopt;
    }
    const size_t start = out.size() - offset;
    for (size_t i = 0; i < length; ++i) {
      out.push_back(out[start + i]);
    }
  }
  if (out.size() != out_size) {
    return std::nullopt;
  }
  return out;
}

struct Entry {
  std::string name;
  uint32_t offset, size;
  size_t field;  // position of the offset field in the directory
};

std::optional<std::vector<Entry>> Directory(const Bytes& big) {
  if (big.size() < 16 || (std::memcmp(big.data(), "BIGF", 4) && std::memcmp(big.data(), "BIG4", 4))) {
    return std::nullopt;
  }
  const uint32_t count = LoadBE32(big, 8);
  std::vector<Entry> out;
  size_t p = 16;
  for (uint32_t i = 0; i < count; ++i) {
    if (p + 8 > big.size()) {
      return std::nullopt;
    }
    Entry e{{}, LoadBE32(big, p), LoadBE32(big, p + 4), p};
    size_t end = p + 8;
    while (end < big.size() && big[end]) {
      ++end;
    }
    if (end >= big.size() || uint64_t(e.offset) + e.size > big.size()) {
      return std::nullopt;
    }
    e.name.assign(reinterpret_cast<const char*>(big.data() + p + 8), end - p - 8);
    out.push_back(std::move(e));
    p = end + 1;
  }
  return out;
}

std::optional<Bytes> ReadFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return std::nullopt;
  }
  Bytes data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return f.bad() ? std::nullopt : std::optional<Bytes>(std::move(data));
}

bool WriteFile(const std::filesystem::path& path, const void* data, size_t size) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(static_cast<const char*>(data), std::streamsize(size));
  return bool(f);
}

std::string Version() {
  return fmt::format("{:08X}", kPatchVersion);
}

// The patched data0.big and data0.bh, or an error message.
std::string Build(Bytes& big, Bytes& bh) {
  const auto directory = Directory(big);
  if (!directory) {
    return "data0.big is not a BIG archive";
  }
  for (const Screen& screen : kScreens) {
    size_t k = 0;
    while (k < directory->size() && (*directory)[k].name != screen.entry) {
      ++k;
    }
    if (k == directory->size()) {
      return fmt::format("{} is missing", screen.entry);
    }
    const Entry& entry = (*directory)[k];
    auto blob = Unpack(Bytes(big.begin() + entry.offset, big.begin() + entry.offset + entry.size));
    if (!blob || blob->size() != screen.size || Crc32(*blob) != screen.crc) {
      return fmt::format("{} is not the expected version", screen.entry);
    }
    for (size_t r = 0; r < screen.run_count; ++r) {
      const Run& run = screen.runs[r];
      std::memcpy(blob->data() + run.offset, run.bytes, run.size);
    }
    const size_t bh_entry = 16 + 16 * k;
    if (bh.size() < bh_entry + 12 || LoadBE32(bh, bh_entry) != entry.offset) {
      return fmt::format("data0.bh does not match data0.big at {}", screen.entry);
    }
    const uint32_t offset = uint32_t((big.size() + 63) & ~size_t(63));
    big.resize(offset);
    big.insert(big.end(), blob->begin(), blob->end());
    StoreBE32(big, entry.field, offset);
    StoreBE32(big, entry.field + 4, uint32_t(blob->size()));
    StoreBE32(bh, bh_entry, offset);
    StoreBE32(bh, bh_entry + 4, uint32_t(blob->size()));
    StoreBE32(bh, bh_entry + 8, 0);
  }
  const uint32_t total = uint32_t(big.size());
  for (int i = 0; i < 4; ++i) {
    big[4 + i] = uint8_t(total >> (8 * i));
  }
  return {};
}

}  // namespace

void ApplyUiPatches(const std::filesystem::path& game_dir) {
  namespace fs = std::filesystem;
  const fs::path big_path = game_dir / "data0.big", bh_path = game_dir / "data0.bh";
  const fs::path big_orig = game_dir / "data0.big.orig", bh_orig = game_dir / "data0.bh.orig";
  const fs::path marker = game_dir / kMarker;
  std::error_code ec;
  if (!fs::is_regular_file(big_path, ec) || !fs::is_regular_file(bh_path, ec)) {
    REXLOG_WARN("[ui] {} has no data0.big; front-end mod not applied", game_dir.string());
    return;
  }
  if (const auto applied = ReadFile(marker);
      applied && std::string(applied->begin(), applied->end()) == Version()) {
    return;
  }
  const bool have_orig = fs::is_regular_file(big_orig, ec) && fs::is_regular_file(bh_orig, ec);
  auto big = ReadFile(have_orig ? big_orig : big_path);
  auto bh = ReadFile(have_orig ? bh_orig : bh_path);
  if (!big || !bh) {
    REXLOG_ERROR("[ui] could not read data0.big / data0.bh; front-end mod not applied");
    return;
  }
  if (const std::string error = Build(*big, *bh); !error.empty()) {
    REXLOG_ERROR("[ui] front-end mod not applied: {}", error);
    return;
  }
  if (!have_orig && (!fs::copy_file(big_path, big_orig, fs::copy_options::overwrite_existing, ec) ||
                     !fs::copy_file(bh_path, bh_orig, fs::copy_options::overwrite_existing, ec))) {
    REXLOG_ERROR("[ui] could not back up data0.big / data0.bh ({}); front-end mod not applied",
                 ec.message());
    return;
  }
  const fs::path big_tmp = game_dir / "data0.big.tmp", bh_tmp = game_dir / "data0.bh.tmp";
  if (!WriteFile(big_tmp, big->data(), big->size()) || !WriteFile(bh_tmp, bh->data(), bh->size())) {
    REXLOG_ERROR("[ui] could not write to {}; front-end mod not applied", game_dir.string());
    fs::remove(big_tmp, ec);
    fs::remove(bh_tmp, ec);
    return;
  }
  fs::rename(big_tmp, big_path, ec);
  if (!ec) {
    fs::rename(bh_tmp, bh_path, ec);
  }
  if (ec) {
    REXLOG_ERROR("[ui] could not replace data0.big / data0.bh ({}); front-end mod not applied",
                 ec.message());
    return;
  }
  const std::string version = Version();
  WriteFile(marker, version.data(), version.size());
  REXLOG_INFO("[ui] front-end mod {} applied to {}", version, game_dir.string());
}

}  // namespace nhl3on3
