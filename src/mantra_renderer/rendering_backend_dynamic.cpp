// RenderingBackend on dynamic rendering (Vulkan 1.3 or VK_KHR_dynamic_rendering).
#include "rendering_backend.h"

#include <rex/ui/vulkan/device.h>

#include "mantra_log.h"

namespace mantra {

std::unique_ptr<RenderingBackend> CreateRenderPassBackend(
    const rex::ui::vulkan::VulkanDevice* device, uint32_t slot_count);

namespace {

class DynamicRenderingBackend final : public RenderingBackend {
 public:
  explicit DynamicRenderingBackend(const rex::ui::vulkan::VulkanDevice* device)
      : device_(device) {}

  const char* name() const override { return "dynamic rendering"; }

  bool PreparePipeline(const PassFormats& formats, VkGraphicsPipelineCreateInfo& info,
                       PipelineTarget& target) override {
    for (uint32_t i = 0; i < formats.color_count; ++i) target.colors[i] = formats.color[i];
    target.rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    target.rendering.pNext = info.pNext;
    target.rendering.colorAttachmentCount = formats.color_count;
    target.rendering.pColorAttachmentFormats = target.colors;
    target.rendering.depthAttachmentFormat = formats.depth_stencil;
    target.rendering.stencilAttachmentFormat = formats.depth_stencil;
    info.pNext = &target.rendering;
    info.renderPass = VK_NULL_HANDLE;
    return true;
  }

  void Begin(VkCommandBuffer cmd, const PassBegin& pass) override {
    VkRenderingAttachmentInfo colors[kMaxPassColors] = {};
    for (uint32_t i = 0; i < pass.formats.color_count; ++i) {
      VkRenderingAttachmentInfo& a = colors[i];
      a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      a.imageView = pass.color[i].view;
      a.imageLayout = pass.color[i].view ? pass.color[i].layout
                                         : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = pass.depth_stencil.view;
    depth.imageLayout = pass.depth_stencil.layout;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = pass.area;
    info.layerCount = 1;
    info.colorAttachmentCount = pass.formats.color_count;
    info.pColorAttachments = colors;
    if (pass.depth_stencil.view) {
      info.pDepthAttachment = &depth;
      info.pStencilAttachment = &depth;
    }
    device_->functions().vkCmdBeginRendering(cmd, &info);
  }

  void End(VkCommandBuffer cmd) override { device_->functions().vkCmdEndRendering(cmd); }

  void Shutdown() override {}

 private:
  const rex::ui::vulkan::VulkanDevice* device_;
};

}  // namespace

std::unique_ptr<RenderingBackend> CreateRenderingBackend(
    const rex::ui::vulkan::VulkanDevice* device, uint32_t slot_count) {
  static const bool force_render_pass = [] {
    const char* v = std::getenv("MANTRA_FORCE_RENDER_PASS");
    return v && *v && *v != '0';
  }();
  std::unique_ptr<RenderingBackend> backend;
  if (device->properties().dynamicRendering && !force_render_pass) {
    backend = std::make_unique<DynamicRenderingBackend>(device);
  } else {
    backend = CreateRenderPassBackend(device, slot_count);
  }
  MANTRA_INFO("rendering: {}{}", backend->name(),
              force_render_pass ? " (MANTRA_FORCE_RENDER_PASS)" : "");
  return backend;
}

}  // namespace mantra
