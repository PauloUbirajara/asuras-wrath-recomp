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
  // In guest pixels. Render targets, resolved surfaces and textures copied
  // from them are allocated `rscale` / kScaleOne times larger (RenderScaleUnits()), so
  // host_width/host_height are the real image size; rects given to Vulkan go
  // through HostRect().
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t host_width = 0;
  uint32_t host_height = 0;
  uint32_t rscale = 40;  // kScaleOne
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
                        TrackedImage& out, uint32_t rscale = 40);
void DestroyTrackedImage(const rex::ui::vulkan::VulkanDevice* device, TrackedImage& image);

VkImageView CreateImageView(const rex::ui::vulkan::VulkanDevice* device, VkImage image,
                            VkFormat format, VkImageAspectFlags aspect);

// Emits a barrier moving the image to (layout, stage, access). `discard` lets
// the old contents go (transition from UNDEFINED).
void TransitionImage(const rex::ui::vulkan::VulkanDevice* device, VkCommandBuffer cmd,
                     TrackedImage& image, VkImageLayout layout, VkPipelineStageFlags stage,
                     VkAccessFlags access, bool discard = false);

// Internal resolution: host pixels per guest pixel, in kScaleOne units
// (kScaleOne = the game's 1280x720; 1/40 = 2.5% steps). From
// MANTRA_RENDER_SCALE at start (a factor "1.5", or a target height "1080"),
// then mantra_set_render_scale (render_scale.h); 2 (5%) to 160 (4x). A change takes
// effect at the next frame (ApplyPendingRenderScale, at swap): render targets
// are recreated at the new scale as they are next used.
constexpr uint32_t kScaleOne = 40;
constexpr uint32_t kMinScaleUnits = 2, kMaxScaleUnits = 160;
uint32_t RenderScaleUnits();
// Takes a pending mantra_set_render_scale; returns whether the scale changed.
bool ApplyPendingRenderScale();
// Guest -> host pixels (coordinates floor, sizes cover the whole guest size).
inline uint32_t HostCoord(uint32_t guest, uint32_t rscale) { return guest * rscale / kScaleOne; }
inline uint32_t HostSize(uint32_t guest, uint32_t rscale) { return (guest * rscale + kScaleOne - 1) / kScaleOne; }
inline VkRect2D HostRect(const VkRect2D& guest, uint32_t rscale) {
  if (rscale == kScaleOne) return guest;
  const uint32_t x0 = HostCoord(uint32_t(guest.offset.x), rscale);
  const uint32_t y0 = HostCoord(uint32_t(guest.offset.y), rscale);
  const uint32_t x1 = HostCoord(uint32_t(guest.offset.x) + guest.extent.width, rscale);
  const uint32_t y1 = HostCoord(uint32_t(guest.offset.y) + guest.extent.height, rscale);
  return {{int32_t(x0), int32_t(y0)}, {x1 - x0, y1 - y0}};
}

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
