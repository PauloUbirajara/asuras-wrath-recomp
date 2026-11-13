#pragma once
// Small Vulkan helpers shared by the mantra render-target pool, the resolved
// surface registry and the copy pipelines: images that remember their current
// layout/stage/access so barriers can be emitted on demand.
#include <cstdint>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace mantra {

struct TrackedImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  uint32_t width = 0;
  uint32_t height = 0;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  VkAccessFlags access = 0;
};

// Creates a 2D, single-mip, single-layer image with dedicated device-local
// memory. `view_formats` (optional) makes the image mutable-format.
bool CreateTrackedImage(const rex::ui::vulkan::VulkanDevice* device, VkFormat format,
                        VkImageAspectFlags aspect, uint32_t width, uint32_t height,
                        VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                        const VkFormat* view_formats, uint32_t view_format_count,
                        TrackedImage& out);
void DestroyTrackedImage(const rex::ui::vulkan::VulkanDevice* device, TrackedImage& image);

VkImageView CreateImageView(const rex::ui::vulkan::VulkanDevice* device, VkImage image,
                            VkFormat format, VkImageAspectFlags aspect);

// Emits a barrier moving the image to (layout, stage, access). `discard` lets
// the old contents go (transition from UNDEFINED).
void TransitionImage(const rex::ui::vulkan::VulkanDevice* device, VkCommandBuffer cmd,
                     TrackedImage& image, VkImageLayout layout, VkPipelineStageFlags stage,
                     VkAccessFlags access, bool discard = false);

// Common attachment/sampling states.
inline constexpr VkPipelineStageFlags kColorAttachmentStage =
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
inline constexpr VkAccessFlags kColorAttachmentAccess =
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
inline constexpr VkPipelineStageFlags kDepthAttachmentStage =
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
inline constexpr VkAccessFlags kDepthAttachmentAccess =
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

}  // namespace mantra
