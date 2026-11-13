#include "shader_cache.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

#include "mantra_log.h"
#include "shader_key.h"

namespace mantra {

std::filesystem::path ShaderCache::DefaultPath() {
  if (const char* env = std::getenv("MANTRA_SHADER_CACHE"); env && *env) {
    return env;
  }
  std::filesystem::path base;
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    base = xdg;
  } else if (const char* home = std::getenv("HOME"); home && *home) {
    base = std::filesystem::path(home) / ".cache";
  } else {
    return {};
  }
  return base / "asura_wrath" / "mantra_shaders.mshc";
}

bool ShaderCache::Load(const std::filesystem::path& path) {
  using namespace shader_cache;
  entries_.clear();
  by_exact_.clear();
  by_normalized_.clear();

  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  data_.assign(std::istreambuf_iterator<char>(file), {});

  CacheHeader header;
  if (data_.size() < sizeof(header)) {
    MANTRA_WARN("shader cache {} is truncated", path.string());
    return false;
  }
  std::memcpy(&header, data_.data(), sizeof(header));
  if (std::memcmp(header.magic, kMagic, 4) || header.version != kVersion) {
    MANTRA_WARN("shader cache {} has an unknown format (version {}); rerun mantra_shadergen",
                path.string(), header.version);
    return false;
  }
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
    if (!(e.flags & kEntryFailed) &&
        (uint64_t(e.spirv_offset) + e.spirv_size > blobs_size_ ||
         uint64_t(e.vertex_elements_offset) + uint64_t(e.vertex_element_count) * 4 > blobs_size_)) {
      MANTRA_WARN("shader cache {}: entry {} points past the end", path.string(), i);
      entries_.clear();
      by_exact_.clear();
      by_normalized_.clear();
      return false;
    }
    by_exact_.emplace(e.exact_hash, i);
    if (!(e.flags & (kEntryFailed | kEntryPixelShader))) {
      by_normalized_.emplace(e.normalized_hash, i);
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
      return make(it->second, Match::kNormalized);
    }
  }
  return result;
}

}  // namespace mantra
