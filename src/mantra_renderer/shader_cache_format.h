#pragma once
// Per-user shader cache written by mantra_shadergen and read by mantra.
// Little-endian host layout:
//   CacheHeader
//   CacheEntry[entry_count]   (sorted by exact_hash)
//   blobs                     (offsets relative to the end of the entry table):
//                             SPIR-V, and per vertex shader its VertexElement
//                             words

#include <cstdint>

namespace mantra::shader_cache {

constexpr char kMagic[4] = {'M', 'S', 'H', 'C'};
// 2: mantra_shader_common.hlsl interface (descriptors instead of buffer
//    device addresses), texture_slot_mask / bool_mask.
// 3: vertex elements, PS outputs; the VS applies the host NDC transform
//    (SharedConstants ndc_scale / ndc_offset) instead of the half-pixel offset.
constexpr uint32_t kVersion = 4;

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
  uint32_t ps_outputs;         // pixel shaders: bits 0..3 = oC0..3, bit 4 = oDepth
  uint32_t vertex_elements_offset;  // blob offset of vertex_element_count words
  uint32_t vertex_element_count;
};
static_assert(sizeof(CacheEntry) == 56);

// D3D declaration usages (XenosRecomp's DeclUsage).
enum class DeclUsage : uint32_t {
  kPosition = 0, kBlendWeight = 1, kBlendIndices = 2, kNormal = 3, kPointSize = 4,
  kTexCoord = 5, kTangent = 6, kBinormal = 7, kTessFactor = 8, kPositionT = 9,
  kColor = 10, kFog = 11, kDepth = 12, kSample = 13,
};

// One vertex shader input, packed in a uint32:
//   bits  0..11 ucode instruction address of the vfetch that reads it
//   bits 12..16 Vulkan input location
//   bits 17..20 DeclUsage, bits 21..24 usage index
//   bit  25     declared uint4 in the shader (else float4)
// `address` is the vertex fetch instruction in the container's microcode and
// `dest` the register it writes: the driver may reorder fetches, so the
// runtime finds the element's fetch by its destination register (v4).
struct VertexElement {
  uint32_t address, location, usage_index;
  DeclUsage usage;
  bool is_uint;
  uint32_t dest;
  static VertexElement Unpack(uint32_t v) {
    return {v & 0xFFF, (v >> 12) & 0x1F, (v >> 21) & 0xF, DeclUsage((v >> 17) & 0xF),
            ((v >> 25) & 1) != 0, v >> 26};
  }
  uint32_t Pack() const {
    return (address & 0xFFF) | ((location & 0x1F) << 12) | ((uint32_t(usage) & 0xF) << 17) |
           ((usage_index & 0xF) << 21) | (uint32_t(is_uint) << 25) | ((dest & 0x3F) << 26);
  }
};

}  // namespace mantra::shader_cache
