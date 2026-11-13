#include "vk_image.h"

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace mantra {

namespace vk = rex::ui::vulkan;

bool CreateTrackedImage(const vk::VulkanDevice* device, VkFormat format,
                        VkImageAspectFlags aspect, uint32_t width, uint32_t height,
                        VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                        const VkFormat* view_formats, uint32_t view_format_count,
                        TrackedImage& out) {
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  VkImageFormatListCreateInfo format_list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
  if (view_format_count) {
    info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (device->extensions().ext_1_2_KHR_image_format_list) {
      format_list.viewFormatCount = view_format_count;
      format_list.pViewFormats = view_formats;
      info.pNext = &format_list;
    }
  }
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  out = TrackedImage{};
  if (!vk::util::CreateDedicatedAllocationImage(device, info, vk::util::MemoryPurpose::kDeviceLocal,
                                                out.image, out.memory)) {
    return false;
  }
  out.format = format;
  out.aspect = aspect;
  out.width = width;
  out.height = height;
  out.samples = samples;
  return true;
}

void DestroyTrackedImage(const vk::VulkanDevice* device, TrackedImage& image) {
  const auto& dfn = device->functions();
  if (image.image != VK_NULL_HANDLE) {
    dfn.vkDestroyImage(device->device(), image.image, nullptr);
  }
  if (image.memory != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device->device(), image.memory, nullptr);
  }
  image = TrackedImage{};
}

VkImageView CreateImageView(const vk::VulkanDevice* device, VkImage image, VkFormat format,
                            VkImageAspectFlags aspect) {
  VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = image;
  info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  info.format = format;
  info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  info.subresourceRange = vk::util::InitializeSubresourceRange(aspect);
  VkImageView view = VK_NULL_HANDLE;
  if (device->functions().vkCreateImageView(device->device(), &info, nullptr, &view) !=
      VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return view;
}

void TransitionImage(const vk::VulkanDevice* device, VkCommandBuffer cmd, TrackedImage& image,
                     VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access,
                     bool discard) {
  const bool layout_changes = discard || image.layout != layout;
  // Read-after-read needs no barrier; anything involving a write does.
  constexpr VkAccessFlags kWriteMask =
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
      VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  if (!layout_changes && !(image.access & kWriteMask) && !(access & kWriteMask)) {
    image.stage |= stage;
    image.access |= access;
    return;
  }
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = discard ? 0 : image.access;
  barrier.dstAccessMask = access;
  barrier.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : image.layout;
  barrier.newLayout = layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = vk::util::InitializeSubresourceRange(image.aspect);
  device->functions().vkCmdPipelineBarrier(cmd, image.stage, stage, 0, 0, nullptr, 0, nullptr, 1,
                                           &barrier);
  image.layout = layout;
  image.stage = stage;
  image.access = access;
}

}  // namespace mantra
