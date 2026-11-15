#pragma once
// Host side of the translated shaders' resource interface
// (tools/mantra_shadergen/mantra_shader_common.hlsl):
//
//   push constants (VS+PS): DrawConstantOffsets, byte offsets into the
//     constant ring of this draw's VS float constants, PS float constants
//     and SharedConstants.
//   set 0 binding 0: storage buffer, the constant ring (all frame slots).
//     The ring also holds the draws' vertex and index data (Allocate).
//   set 1 bindings 0..3: Texture2D / Texture3D / TextureCube / sampler arrays,
//     kFetchSlots each, indexed by Xenos fetch constant slot.
//   specialization constant 0: XenosRecomp's spec constant flags.
//
// The constant ring is split into one region per submission slot; a region
// is reused only after its submission completed.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace mantra {

struct DrawConstantOffsets {
  uint32_t vertex = 0;
  uint32_t pixel = 0;
  uint32_t shared = 0;
};

// XenosRecomp's SharedConstants layout (the descriptor index area is unused
// by mantra's header but kept so the offsets match).
struct SharedConstants {
  uint32_t descriptor_indices[64] = {};
  uint32_t booleans = 0;           // VS bool b0..15 in bits 0..15, PS in 16..31
  uint32_t swapped_texcoords = 0;  // bit per TEXCOORD semantic: fetch as .yxwz
  float half_pixel_offset[2] = {};
  float alpha_threshold = 0.0f;
  // mantra: guest vertex index of host vertex index 0 (the VS r0.x).
  uint32_t vertex_index_base = 0;
  // mantra: bit i = color output i goes to a fixed-point 16-bit target; the
  // pixel shader writes it / 32 (the host SNORM image holds -32...32 / 32).
  uint32_t fixed16_output_mask = 0;
  // mantra: bit i = color output i goes to a 7e3 float target (hosted as
  // RGBA16F); the pixel shader clamps it to 0...31.875 (NaN -> 0).
  uint32_t float7e3_output_mask = 0;
  // mantra: host viewport transform applied by the VS
  // (draw_util::ViewportInfo ndc_scale / ndc_offset).
  float ndc_scale[3] = {1.0f, 1.0f, 1.0f};
  float pad2 = 0.0f;
  float ndc_offset[3] = {};
  float pad3 = 0.0f;
};
static_assert(offsetof(SharedConstants, booleans) == 256);
static_assert(offsetof(SharedConstants, half_pixel_offset) == 264);
static_assert(offsetof(SharedConstants, alpha_threshold) == 272);
static_assert(offsetof(SharedConstants, vertex_index_base) == 276);
static_assert(offsetof(SharedConstants, fixed16_output_mask) == 280);
static_assert(offsetof(SharedConstants, float7e3_output_mask) == 284);
static_assert(offsetof(SharedConstants, ndc_scale) == 288);
static_assert(offsetof(SharedConstants, ndc_offset) == 304);

class ShaderInterface {
 public:
  static constexpr uint32_t kFetchSlots = 32;
  // Float constants uploaded per stage (256 float4 registers).
  static constexpr uint32_t kFloatConstantBytes = 256 * 16;
  static constexpr VkDeviceSize kRingBytesPerSlot = VkDeviceSize(32) << 20;

  bool Initialize(const rex::ui::vulkan::VulkanDevice* device, uint32_t frame_slots);
  // Clears the placeholder textures; once, before the first draw.
  void InitializeResources(VkCommandBuffer cmd);
  void Shutdown();

  VkPipelineLayout pipeline_layout() const { return pipeline_layout_; }

  // Starts filling the ring region of submission slot `slot`.
  void BeginSlot(uint32_t slot);
  // Makes the writes of the current region visible to the GPU (before submit).
  void FlushSlot();
  // Copies `size` bytes into the current region. Returns false when the region
  // is full (the caller submits and retries).
  bool Upload(const void* data, uint32_t size, uint32_t& offset);
  // Reserves `size` bytes (16-byte aligned) for the caller to fill. Returns
  // nullptr when the region is full.
  uint8_t* Allocate(uint32_t size, uint32_t& offset);
  VkBuffer ring_buffer() const { return ring_buffer_; }
  VkDeviceSize slot_bytes_used() const { return cursor_ - slot_begin_; }

  // Binds both descriptor sets and pushes the offsets; `textures` null binds
  // the all-placeholder set.
  void Bind(VkCommandBuffer cmd, const DrawConstantOffsets& offsets,
            VkDescriptorSet textures = VK_NULL_HANDLE) const;

  // A textures set (set 1) from the current slot's pool, written with these
  // views (per heap: 0 = 2D, 1 = 3D, 2 = cube) and samplers; null entries
  // get the placeholders. VK_NULL_HANDLE when the pool is exhausted.
  VkDescriptorSet AllocateTextureSet(const VkImageView views[3][kFetchSlots],
                                     const VkSampler samplers[kFetchSlots]);

  // Creates a module from cached SPIR-V (VK_NULL_HANDLE on failure).
  VkShaderModule CreateModule(std::span<const uint8_t> spirv) const;

 private:
  bool CreateLayouts();
  bool CreateRing(uint32_t frame_slots);
  bool CreatePlaceholders();
  bool CreateDescriptorSets();
  static constexpr uint32_t kTextureSetsPerSlot = 1024;

  const rex::ui::vulkan::VulkanDevice* device_ = nullptr;

  VkDescriptorSetLayout constants_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout textures_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;

  VkBuffer ring_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory ring_memory_ = VK_NULL_HANDLE;
  uint32_t ring_memory_type_ = UINT32_MAX;
  VkDeviceSize ring_memory_size_ = 0;
  uint8_t* ring_mapping_ = nullptr;
  VkDeviceSize slot_begin_ = 0;
  VkDeviceSize cursor_ = 0;
  VkDeviceSize flushed_ = 0;

  // Placeholder textures (opaque white) bound to every fetch slot until the
  // texture cache exists; index: 0 = 2D, 1 = 3D, 2 = cube.
  VkImage placeholder_images_[3] = {};
  VkDeviceMemory placeholder_memory_[3] = {};
  VkImageView placeholder_views_[3] = {};
  VkSampler placeholder_sampler_ = VK_NULL_HANDLE;
  bool placeholders_initialized_ = false;

  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet constants_set_ = VK_NULL_HANDLE;
  VkDescriptorSet textures_set_ = VK_NULL_HANDLE;
  // Per submission slot, reset when the slot is reused.
  std::vector<VkDescriptorPool> texture_pools_;
  uint32_t current_slot_ = 0;
};

}  // namespace mantra
