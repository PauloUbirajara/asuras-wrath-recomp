#include "shader_cache.h"

#include <algorithm>

#include <cstdlib>
#include <cstring>
#include <fstream>

#include "mantra_log.h"
#include "shader_key.h"
#include "../platform/platform.h"

namespace mantra {

std::filesystem::path ShaderCache::DefaultPath() {
  if (const char* env = std::getenv("MANTRA_SHADER_CACHE"); env && *env) {
    return env;
  }
  const std::filesystem::path base = asura::platform::CacheDir();
  return base.empty() ? base : base / "mantra_shaders.mshc";
}

bool ShaderCache::Load(const std::filesystem::path& path) {
  using namespace shader_cache;
  entries_.clear();
  by_exact_.clear();
  by_normalized_.clear();

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return false;
  }
  // One read: the cache is hundreds of MB, and a byte-wise copy took seconds.
  const std::streamoff length = file.tellg();
  if (length < 0) {
    return false;
  }
  data_.resize(size_t(length));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(data_.data()), length)) {
    data_.clear();
    return false;
  }

  CacheHeader header;
  if (data_.size() < sizeof(header)) {
    MANTRA_WARN("shader cache {} is truncated", path.string());
    return false;
  }
  std::memcpy(&header, data_.data(), sizeof(header));
  if (std::memcmp(header.magic, kMagic, 4) || header.version < kMinVersion || header.version > kVersion) {
    MANTRA_WARN("shader cache {} has an unknown format (version {}); rerun mantra_shadergen",
                path.string(), header.version);
    return false;
  }
  version_ = header.version;
  const size_t table_end = sizeof(header) + size_t(header.entry_count) * sizeof(CacheEntry);
  if (table_end > data_.size()) {
    MANTRA_WARN("shader cache {} is truncated", path.string());
    return false;
  }
  entries_.resize(header.entry_count);
  std::memcpy(entries_.data(), data_.data() + sizeof(header),
              entries_.size() * sizeof(CacheEntry));
  blobs_ = data_.data() + table_end;
  blobs_size_ = data_.size() - table_end;

  for (size_t i = 0; i < entries_.size(); ++i) {
    const CacheEntry& e = entries_[i];
    const uint64_t swizzle_words =
        version_ >= 7 && !(e.flags & kEntryPixelShader) ? e.vertex_element_count : 0;
    if (!(e.flags & kEntryFailed) &&
        (uint64_t(e.spirv_offset) + e.spirv_size > blobs_size_ ||
         uint64_t(e.vertex_elements_offset) +
                 (uint64_t(e.vertex_element_count) + e.export_count + swizzle_words) * 4 >
             blobs_size_)) {
      MANTRA_WARN("shader cache {}: entry {} points past the end", path.string(), i);
      entries_.clear();
      by_exact_.clear();
      by_normalized_.clear();
      return false;
    }
    by_exact_.emplace(e.exact_hash, i);
    if (!(e.flags & (kEntryFailed | kEntryPixelShader))) {
      by_normalized_[e.normalized_hash].push_back(i);
    }
  }
  return true;
}

ShaderCache::Lookup ShaderCache::Find(const void* microcode_be, size_t byte_count,
                                      bool is_pixel) const {
  using namespace shader_cache;
  Lookup result;
  if (entries_.empty()) {
    return result;
  }
  auto make = [&](size_t index, Match match) {
    const CacheEntry& e = entries_[index];
    result.entry = &e;
    if (e.flags & kEntryFailed) {
      result.match = Match::kFailed;
    } else {
      result.match = match;
      result.spirv = {blobs_ + e.spirv_offset, e.spirv_size};
      result.vertex_elements.resize(e.vertex_element_count);
      std::memcpy(result.vertex_elements.data(), blobs_ + e.vertex_elements_offset,
                  e.vertex_element_count * sizeof(uint32_t));
      result.exports.resize(e.export_count);
      std::memcpy(result.exports.data(),
                  blobs_ + e.vertex_elements_offset + e.vertex_element_count * sizeof(uint32_t),
                  e.export_count * sizeof(uint32_t));
      if (version_ >= 7 && !(e.flags & kEntryPixelShader)) {
        result.vertex_swizzles.resize(e.vertex_element_count);
        std::memcpy(result.vertex_swizzles.data(),
                    blobs_ + e.vertex_elements_offset +
                        (e.vertex_element_count + e.export_count) * sizeof(uint32_t),
                    e.vertex_element_count * sizeof(uint32_t));
      }
    }
    return result;
  };
  if (auto it = by_exact_.find(shader_key::ExactHash(microcode_be, byte_count));
      it != by_exact_.end()) {
    return make(it->second, Match::kExact);
  }
  if (!is_pixel) {
    if (auto it = by_normalized_.find(shader_key::NormalizedHash(microcode_be, byte_count));
        it != by_normalized_.end()) {
      const std::vector<size_t>& candidates = it->second;
      if (candidates.size() == 1) {
        return make(candidates[0], Match::kNormalized);
      }
      // An output the translation doesn't write would leave the pixel
      // shader's input undefined (e.g. a texture coordinate of 0).
      const std::vector<uint32_t> runtime = shader_key::InterpolatorExports(microcode_be, byte_count);
      for (size_t index : candidates) {
        const CacheEntry& e = entries_[index];
        const auto* exports = reinterpret_cast<const uint32_t*>(
            blobs_ + e.vertex_elements_offset + e.vertex_element_count * sizeof(uint32_t));
        const std::span<const uint32_t> translated(exports, e.export_count);
        const bool covers = std::all_of(runtime.begin(), runtime.end(), [&](uint32_t r) {
          return std::any_of(translated.begin(), translated.end(),
                             [&](uint32_t t) { return (t & ~0xFu) == (r & ~0xFu); });
        });
        if (covers) {
          result.candidates = uint32_t(candidates.size());
          return make(index, Match::kNormalized);
        }
      }
      result.candidates = uint32_t(candidates.size());
      result.uncovered = true;
      return make(candidates[0], Match::kNormalized);
    }
  }
  return result;
}

}  // namespace mantra
