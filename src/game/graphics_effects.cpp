#include "graphics_effects.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>

#include "../ui/game_assets.h"
#include "game_files.h"

namespace asura::game {
namespace {

// Coalesced_<LANG>.bin, big-endian: i32 file count, then per file an FString
// name, i32 section count, per section an FString name, i32 key count and
// FString key/value pairs. FString: i32 length counting the terminating zero,
// positive = ANSI bytes, negative = UTF-16LE units.
class Patcher {
 public:
  Patcher(const std::vector<uint8_t>& in, uint32_t off) : in_(in), off_(off) {}

  // The patched file, or empty if `in` doesn't parse. `changed`: values set.
  std::vector<uint8_t> Run(int& changed) {
    changed = 0;
    const int32_t files = I32();
    for (int32_t f = 0; ok_ && f < files; ++f) {
      std::string file = String();
      const int32_t sections = I32();
      for (int32_t s = 0; ok_ && s < sections; ++s) {
        const bool settings = String() == "SystemSettings";
        const int32_t keys = I32();
        for (int32_t k = 0; ok_ && k < keys; ++k) {
          const std::string key = String();
          const size_t value_at = pos_;
          String();
          if (!ok_ || !settings || !TurnedOff(key)) continue;
          // Replace the value just copied with "False", in the same
          // encoding (this game's strings are UTF-16).
          const bool wide = int32_t(uint32_t(in_[value_at]) << 24) < 0;
          out_.resize(out_.size() - (pos_ - value_at));
          static constexpr char kFalse[] = "False";  // with its zero
          PutI32(wide ? -int32_t(sizeof(kFalse)) : int32_t(sizeof(kFalse)));
          for (char c : kFalse) {
            out_.push_back(uint8_t(c));
            if (wide) out_.push_back(0);
          }
          ++changed;
        }
      }
    }
    if (!ok_ || pos_ != in_.size()) return {};
    return std::move(out_);
  }

 private:
  bool TurnedOff(std::string_view key) const {
    for (size_t e = 0; e < kGraphicsEffectCount; ++e) {
      if (!(off_ & (1u << e))) continue;
      for (const char* name : kGraphicsEffects[e].ini_keys) {
        if (name && key == name) return true;
      }
    }
    return false;
  }
  // Reads (and copies) a big-endian i32.
  int32_t I32() {
    if (!ok_ || pos_ + 4 > in_.size()) {
      ok_ = false;
      return 0;
    }
    const uint32_t v = uint32_t(in_[pos_]) << 24 | uint32_t(in_[pos_ + 1]) << 16 |
                       uint32_t(in_[pos_ + 2]) << 8 | in_[pos_ + 3];
    Copy(4);
    return int32_t(v);
  }
  void PutI32(int32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out_.push_back(uint8_t(uint32_t(v) >> shift));
  }
  void Copy(size_t n) {
    out_.insert(out_.end(), in_.begin() + pos_, in_.begin() + pos_ + n);
    pos_ += n;
  }
  // Reads (and copies) an FString; returns its text (UTF-16 narrowed: only
  // ASCII names are looked for).
  std::string String() {
    const int32_t n = I32();
    if (!ok_) return {};
    const size_t bytes = n >= 0 ? size_t(n) : size_t(-int64_t(n)) * 2;
    if (bytes > in_.size() - pos_) {
      ok_ = false;
      return {};
    }
    std::string text;
    if (n > 0) {
      text.assign(reinterpret_cast<const char*>(in_.data() + pos_), size_t(n));
    } else {
      for (size_t i = 0; i + 1 < bytes; i += 2) {
        const uint16_t unit = uint16_t(in_[pos_ + i] | in_[pos_ + i + 1] << 8);
        text.push_back(unit < 0x80 ? char(unit) : '?');
      }
    }
    while (!text.empty() && text.back() == '\0') text.pop_back();
    Copy(bytes);
    return text;
  }

  const std::vector<uint8_t>& in_;
  const uint32_t off_;
  std::vector<uint8_t> out_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace

void ApplyGraphicsEffects(rex::Runtime* runtime, uint32_t off) {
  namespace fs = std::filesystem;
  off &= (1u << kGraphicsEffectCount) - 1;
  if (!off || !runtime) return;
  const fs::path cooked = runtime->game_data_root() / "BCGame" / "CookedXbox360";
  const fs::path cache = asura::ui::GameCacheRoot();
  std::error_code ec;
  if (cache.empty() || !fs::is_directory(cooked, ec)) {
    REXLOG_WARN("Graphics effects: no {} or cache folder", cooked.string());
    return;
  }
  // Rebuilt each start: the settings or the game folder may have changed.
  const fs::path folder = cache / "config";
  fs::remove_all(folder, ec);
  fs::create_directories(folder, ec);
  std::vector<std::string> patched;
  int changed_total = 0;
  for (const auto& entry : fs::directory_iterator(cooked, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("Coalesced_", 0) != 0 || entry.path().extension() != ".bin") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    int changed = 0;
    const std::vector<uint8_t> out = Patcher(data, off).Run(changed);
    if (out.empty()) {
      REXLOG_WARN("Graphics effects: cannot read {}, left as it is", name);
      continue;
    }
    std::ofstream file(folder / name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    if (!file) {
      REXLOG_WARN("Graphics effects: cannot write {}", (folder / name).string());
      continue;
    }
    patched.push_back(name);
    changed_total += changed;
  }
  const size_t replaced =
      ReplaceGameFiles(runtime, "\\Device\\AsuraConfig", folder, "BCGame\\CookedXbox360", patched);
  std::string names;
  for (size_t e = 0; e < kGraphicsEffectCount; ++e) {
    if (off & (1u << e)) names += std::string(names.empty() ? "" : ", ") + kGraphicsEffects[e].key;
  }
  REXLOG_INFO("Graphics effects off: {} ({} settings in {} config files)", names, changed_total,
              replaced);
}

}  // namespace asura::game
