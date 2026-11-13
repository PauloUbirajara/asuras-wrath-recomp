#pragma once
// Read-only view of the shader cache written by mantra_shadergen
// (see shader_cache_format.h). Lookups try the exact microcode hash first,
// then the normalized hash (driver-patched vertex shaders).

#include <cstdint>
#include <filesystem>
#include <span>
#include <unordered_map>
#include <vector>

#include "shader_cache_format.h"

namespace mantra {

class ShaderCache {
 public:
  enum class Match { kNone, kExact, kNormalized, kFailed };

  struct Lookup {
    Match match = Match::kNone;
    const shader_cache::CacheEntry* entry = nullptr;
    std::span<const uint8_t> spirv;
  };

  // Default location: $MANTRA_SHADER_CACHE, else
  // ${XDG_CACHE_HOME:-~/.cache}/asura_wrath/mantra_shaders.mshc.
  static std::filesystem::path DefaultPath();

  bool Load(const std::filesystem::path& path);
  bool loaded() const { return !entries_.empty(); }
  size_t size() const { return entries_.size(); }

  Lookup Find(const void* microcode_be, size_t byte_count, bool is_pixel) const;

 private:
  std::vector<uint8_t> data_;
  std::vector<shader_cache::CacheEntry> entries_;
  const uint8_t* blobs_ = nullptr;
  size_t blobs_size_ = 0;
  std::unordered_map<uint64_t, size_t> by_exact_;
  // Only translated vertex shaders; first one wins on collisions.
  std::unordered_map<uint64_t, size_t> by_normalized_;
};

}  // namespace mantra
