#pragma once
// Per-user shader cache written by mantra_shadergen and read by mantra.
// Little-endian host layout:
//   CacheHeader
//   CacheEntry[entry_count]   (sorted by exact_hash)
//   SPIR-V blobs              (offsets relative to the end of the entry table)

#include <cstdint>

namespace mantra::shader_cache {

constexpr char kMagic[4] = {'M', 'S', 'H', 'C'};
// 2: mantra_shader_common.hlsl interface (descriptors instead of buffer
//    device addresses), texture_slot_mask / bool_mask.
constexpr uint32_t kVersion = 2;

struct CacheHeader {
  char magic[4];
  uint32_t version;
  uint32_t entry_count;
  uint32_t reserved;
};

enum EntryFlags : uint32_t {
  kEntryPixelShader = 1u << 0,
  // Known shader that failed to translate (spirv_size is 0).
  kEntryFailed = 1u << 1,
};

struct CacheEntry {
  uint64_t exact_hash;       // shader_key::ExactHash of the container microcode
  uint64_t normalized_hash;  // shader_key::NormalizedHash
  uint32_t flags;            // EntryFlags
  uint32_t spec_constants_mask;
  uint32_t spirv_offset;
  uint32_t spirv_size;
  uint32_t microcode_dwords;
  uint32_t texture_slot_mask;  // fetch constant slots sampled (bit per slot)
  uint32_t bool_mask;          // bool constant registers tested (bit per register)
  uint32_t reserved;
};
static_assert(sizeof(CacheEntry) == 48);

}  // namespace mantra::shader_cache
