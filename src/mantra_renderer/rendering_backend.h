#pragma once
// How mantra opens and closes a render pass instance. Two implementations,
// one picked per device by CreateRenderingBackend:
//   rendering_backend_dynamic.cpp      dynamic rendering (Vulkan 1.3, or 1.1/1.2
//                                      with VK_KHR_dynamic_rendering)
//   rendering_backend_render_pass.cpp  VkRenderPass + VkFramebuffer, for
//                                      drivers without it (many Android ones)
// MANTRA_FORCE_RENDER_PASS=1 picks the second one anywhere, for testing.
//
// Pipelines and passes describe their attachments the same way for both: the
// callers never touch VkRenderingInfo or VkRenderPass themselves.
#include <cstdint>
#include <memory>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace mantra {

inline constexpr uint32_t kMaxPassColors = 4;

// What a pipeline draws into (render pass compatibility): formats and samples.
struct PassFormats {
  uint32_t color_count = 0;
  VkFormat color[kMaxPassColors] = {};       // VK_FORMAT_UNDEFINED: unused slot
  VkFormat depth_stencil = VK_FORMAT_UNDEFINED;  // used for both aspects
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

struct PassAttachment {
  VkImageView view = VK_NULL_HANDLE;  // VK_NULL_HANDLE: unused slot
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // the image's layout throughout the pass
};

// One pass: attachments are loaded and stored (mantra keeps EDRAM contents in
// them across passes), already in `layout` (the caller transitions them).
struct PassBegin {
  PassFormats formats;
  PassAttachment color[kMaxPassColors];
  PassAttachment depth_stencil;
  // Host pixels. `extent` is at most every attachment's size and covers
  // `area` (the framebuffer size for render passes).
  VkExtent2D extent = {};
  VkRect2D area = {};
};

// Storage that must outlive vkCreateGraphicsPipelines for one pipeline.
struct PipelineTarget {
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkFormat colors[kMaxPassColors] = {};
};

class RenderingBackend {
 public:
  virtual ~RenderingBackend() = default;
  virtual const char* name() const = 0;

  // Points `info` at passes with `formats` (pNext chain or renderPass).
  // False when no pass can be made for them.
  virtual bool PreparePipeline(const PassFormats& formats, VkGraphicsPipelineCreateInfo& info,
                               PipelineTarget& target) = 0;

  virtual void Begin(VkCommandBuffer cmd, const PassBegin& pass) = 0;
  virtual void End(VkCommandBuffer cmd) = 0;

  // Submission slot `slot` is being reused: everything recorded in it has
  // completed. Passes begun from now on belong to it.
  virtual void BeginSlot(uint32_t slot) { (void)slot; }

  virtual void Shutdown() = 0;
};

// Dynamic rendering when the device has it (and MANTRA_FORCE_RENDER_PASS is
// unset), else render passes. `slot_count`: submission slots in flight.
std::unique_ptr<RenderingBackend> CreateRenderingBackend(
    const rex::ui::vulkan::VulkanDevice* device, uint32_t slot_count);

}  // namespace mantra
