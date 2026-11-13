#pragma once
// Graphics pipelines for guest draws: the translated (cached) shaders plus the
// fixed-function state taken from the guest registers, rendering with
// dynamic rendering into the RenderTargetPool images.
//
// Dynamic state: viewport, scissor, depth bias, blend constants, stencil
// compare/write masks and reference.
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace mantra {

inline constexpr uint32_t kMaxVertexBindings = 16;
inline constexpr uint32_t kMaxVertexAttributes = 32;
inline constexpr uint32_t kMaxColorAttachments = 4;

// Everything a pipeline depends on. Compared and hashed bytewise: always
// zero-initialize (padding included) before filling.
struct PipelineKey {
  uint64_t vertex_module;  // VkShaderModule
  uint64_t pixel_module;   // VkShaderModule, 0 = no fragment shader
  uint32_t spec_constants;
  uint32_t depth_format;  // VkFormat, 0 = no depth/stencil attachment
  uint32_t color_formats[kMaxColorAttachments];  // VkFormat, 0 = unused
  uint8_t color_count;    // attachments up to the last used one
  uint8_t sample_count;   // VkSampleCountFlagBits
  uint8_t topology;       // VkPrimitiveTopology
  uint8_t rect_list;      // expand rectangles in the geometry shader
  uint8_t primitive_restart;
  uint8_t polygon_mode;   // VkPolygonMode
  uint8_t cull_mode;      // VkCullModeFlags
  uint8_t front_face;     // VkFrontFace
  uint8_t depth_bias;
  uint8_t depth_test;
  uint8_t depth_write;
  uint8_t depth_compare;  // VkCompareOp
  uint8_t stencil_test;
  uint8_t stencil_front[4];  // fail, pass, depth fail (VkStencilOp), compare (VkCompareOp)
  uint8_t stencil_back[4];
  uint8_t alpha_to_coverage;
  struct Blend {
    uint8_t enable;
    uint8_t src_color, dst_color, color_op;  // VkBlendFactor / VkBlendOp
    uint8_t src_alpha, dst_alpha, alpha_op;
    uint8_t write_mask;  // VkColorComponentFlags
  } blend[kMaxColorAttachments];
  uint8_t binding_count;
  uint8_t attribute_count;
  uint8_t pad[2];
  uint32_t binding_strides[kMaxVertexBindings];  // bytes
  struct Attribute {
    uint8_t location;
    uint8_t binding;
    uint16_t offset;  // bytes
    uint32_t format;  // VkFormat
  } attributes[kMaxVertexAttributes];

  bool operator==(const PipelineKey& other) const {
    return std::memcmp(this, &other, sizeof(*this)) == 0;
  }
};

class DrawPipelines {
 public:
  bool Initialize(const rex::ui::vulkan::VulkanDevice* device, VkPipelineLayout layout);
  void Shutdown();

  // Finds or creates the pipeline (VK_NULL_HANDLE if creation failed; failures
  // are remembered).
  VkPipeline Get(const PipelineKey& key);

  size_t size() const { return pipelines_.size(); }
  uint64_t creation_failures() const { return creation_failures_; }
  bool rect_lists_supported() const { return rect_list_gs_ != VK_NULL_HANDLE; }

 private:
  VkPipeline Create(const PipelineKey& key);

  struct KeyHash {
    size_t operator()(const PipelineKey& key) const;
  };

  const rex::ui::vulkan::VulkanDevice* device_ = nullptr;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkShaderModule rect_list_gs_ = VK_NULL_HANDLE;
  std::unordered_map<PipelineKey, VkPipeline, KeyHash> pipelines_;
  uint64_t creation_failures_ = 0;
};

}  // namespace mantra
