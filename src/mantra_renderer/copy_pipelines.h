#pragma once
// Texel-copy pipelines (shaders/copy.frag) used for EDRAM resolves and for
// presenting a resolved frontbuffer. One graphics pipeline per destination
// format and source sample mode, rendered with dynamic rendering.
#include <cstdint>
#include <unordered_map>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace mantra {

struct CopyPushConstants {
  int32_t src_offset[2] = {0, 0};
  uint32_t sample_mask = 1;
  float scale = 1.0f;
  // Per output component: 0-3 = source component, 4 = 0.0, 5 = 1.0.
  uint32_t swizzle[4] = {0, 1, 2, 3};
};
static_assert(sizeof(CopyPushConstants) == 32);

struct CopyRect {
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
};

class CopyPipelines {
 public:
  bool Initialize(const rex::ui::vulkan::VulkanDevice* device);
  void Shutdown();

  VkDescriptorSetLayout descriptor_set_layout() const { return set_layout_; }

  // Records dst[rect] = copy(src) into `cmd`. The caller transitions src to
  // SHADER_READ_ONLY_OPTIMAL and dst to COLOR_ATTACHMENT_OPTIMAL first.
  // `descriptor_pool` must be reset only after `cmd` completes.
  bool Record(VkCommandBuffer cmd, VkDescriptorPool descriptor_pool, VkImageView src_view,
              bool src_multisampled, VkImageView dst_view, VkFormat dst_format,
              uint32_t dst_width, uint32_t dst_height, const CopyRect& rect,
              const CopyPushConstants& constants);

 private:
  VkPipeline GetPipeline(VkFormat dst_format, bool src_multisampled);

  const rex::ui::vulkan::VulkanDevice* device_ = nullptr;
  VkShaderModule vs_ = VK_NULL_HANDLE;
  VkShaderModule fs_ = VK_NULL_HANDLE;
  VkShaderModule fs_ms_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
};

}  // namespace mantra
