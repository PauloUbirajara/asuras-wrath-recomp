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
    // Packed shader_cache::VertexElement words (vertex shaders).
    std::vector<uint32_t> vertex_elements;
    // shader_key::InterpolatorExports of the translated microcode (vertex
    // shaders, cache v6).
    std::vector<uint32_t> exports;
    // Per vertex element, the destination swizzle of its fetch in the
    // translated (container) microcode (vertex shaders, cache v7).
    std::vector<uint32_t> vertex_swizzles;
    // v10: vertex_swizzles carry shader_cache::kSwizzlePackedFormat.
    bool container_formats = false;
    // Before v11: `exports` record scalar exports the old way (see
    // shader_key::InterpolatorExports).
    bool legacy_exports = false;
    // Normalized matches: how many game shaders shared the hash, and whether
    // none of them exports everything the runtime code does.
    uint32_t candidates = 0;
    bool uncovered = false;
  };

  // Default location: $MANTRA_SHADER_CACHE, else
  // ${XDG_CACHE_HOME:-~/.cache}/asura_wrath_recomp/mantra_shaders.mshc.
  static std::filesystem::path DefaultPath();

  bool Load(const std::filesystem::path& path);
  bool loaded() const { return !entries_.empty(); }
  size_t size() const { return entries_.size(); }
  uint32_t version() const { return version_; }

  Lookup Find(const void* microcode_be, size_t byte_count, bool is_pixel) const;

 private:
  std::vector<uint8_t> data_;
  uint32_t version_ = 0;
  std::vector<shader_cache::CacheEntry> entries_;
  const uint8_t* blobs_ = nullptr;
  size_t blobs_size_ = 0;
  std::unordered_map<uint64_t, size_t> by_exact_;
  // Only translated vertex shaders, in entry order. Game shaders that differ
  // only in their exports share a normalized hash; Find takes the first one
  // whose translation exports everything the runtime code does.
  std::unordered_map<uint64_t, std::vector<size_t>> by_normalized_;
};

}  // namespace mantra
