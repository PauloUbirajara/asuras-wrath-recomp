#pragma once
// Texel-copy pipelines (shaders/copy.frag) used for EDRAM resolves and for
// presenting a resolved frontbuffer. One graphics pipeline per destination
// format and source sample mode, rendered through the RenderingBackend.
#include <cstdint>
#include <unordered_map>

#include <rex/ui/vulkan/api.h>

#include "rendering_backend.h"
#include "vk_image.h"

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
  // 0 = copy, 1 = float -> 7e3 bits (10:10:10:2 unorm), 2 = the reverse.
  uint32_t mode = 0;
  // Source pixels per destination pixel (render scales differ), and whether
  // to filter bilinearly (else the nearest texel).
  float src_ratio = 1.0f;
  uint32_t linear = 0;
  uint32_t padding = 0;
};
static_assert(sizeof(CopyPushConstants) == 48);

struct CopyRect {
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
};

class CopyPipelines {
 public:
  bool Initialize(const rex::ui::vulkan::VulkanDevice* device, RenderingBackend* rendering);
  void Shutdown();

  VkDescriptorSetLayout descriptor_set_layout() const { return set_layout_; }

  // Records dst[rect] = copy(src) into `cmd`. The caller transitions src to
  // SHADER_READ_ONLY_OPTIMAL and dst to COLOR_ATTACHMENT_OPTIMAL first.
  // `descriptor_pool` must be reset only after `cmd` completes. `rect`,
  // `dst_width`/`dst_height` and `constants.src_offset` are in guest pixels;
  // the destination is `rscale` / kScaleOne larger (RenderScaleUnits()), the source
  // `src_rscale` / kScaleOne (0: the same). Between different scales, `linear`
  // filters bilinearly (single-sampled sources only), else nearest.
  bool Record(VkCommandBuffer cmd, VkDescriptorPool descriptor_pool, VkImageView src_view,
              bool src_multisampled, VkImageView dst_view, VkFormat dst_format,
              uint32_t dst_width, uint32_t dst_height, const CopyRect& rect,
              const CopyPushConstants& constants, uint32_t rscale = RenderScaleUnits(),
              uint32_t src_rscale = 0, bool linear = false);

 private:
  VkPipeline GetPipeline(VkFormat dst_format, bool src_multisampled);

  const rex::ui::vulkan::VulkanDevice* device_ = nullptr;
  RenderingBackend* rendering_ = nullptr;
  VkShaderModule vs_ = VK_NULL_HANDLE;
  VkShaderModule fs_ = VK_NULL_HANDLE;
  VkShaderModule fs_ms_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
};

}  // namespace mantra
