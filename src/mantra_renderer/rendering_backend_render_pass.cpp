// RenderingBackend on VkRenderPass + VkFramebuffer, for drivers without
// dynamic rendering (Vulkan 1.0-1.2 without VK_KHR_dynamic_rendering).
//
// Render passes are cached forever (a few dozen format/layout combinations).
// Framebuffers name image views, which mantra recycles, so they are made per
// submission slot and destroyed when the slot is reused: by then the slot's
// commands have completed and every view they named was still alive.
#include <array>
#include <map>
#include <mutex>
#include <vector>

#include <rex/ui/vulkan/device.h>

#include "mantra_log.h"
#include "rendering_backend.h"

namespace mantra {

namespace {

bool HasStencil(VkFormat format) {
  switch (format) {
    case VK_FORMAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return true;
    default:
      return false;
  }
}

class RenderPassBackend final : public RenderingBackend {
 public:
  RenderPassBackend(const rex::ui::vulkan::VulkanDevice* device, uint32_t slot_count)
      : device_(device), slots_(slot_count) {}
  ~RenderPassBackend() override { Shutdown(); }

  const char* name() const override { return "render passes"; }

  bool PreparePipeline(const PassFormats& formats, VkGraphicsPipelineCreateInfo& info,
                       PipelineTarget&) override {
    // Layouts don't affect render pass compatibility: the usual ones.
    PassAttachment colors[kMaxPassColors];
    for (uint32_t i = 0; i < formats.color_count; ++i) {
      colors[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    const VkRenderPass pass =
        GetRenderPass(formats, colors, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    info.renderPass = pass;
    info.subpass = 0;
    return pass != VK_NULL_HANDLE;
  }

  void Begin(VkCommandBuffer cmd, const PassBegin& pass) override {
    // Slots without a view stay unused in the pass (VK_ATTACHMENT_UNUSED).
    PassFormats formats = pass.formats;
    for (uint32_t i = 0; i < formats.color_count; ++i) {
      if (!pass.color[i].view) formats.color[i] = VK_FORMAT_UNDEFINED;
    }
    if (!pass.depth_stencil.view) formats.depth_stencil = VK_FORMAT_UNDEFINED;
    const VkRenderPass render_pass =
        GetRenderPass(formats, pass.color, pass.depth_stencil.layout);
    if (!render_pass) {
      MANTRA_WARN_ONCE("rendering: no render pass for these attachments; pass skipped");
      began_ = false;
      return;
    }
    const VkFramebuffer framebuffer = GetFramebuffer(render_pass, formats, pass);
    if (!framebuffer) {
      MANTRA_WARN_ONCE("rendering: vkCreateFramebuffer failed; pass skipped");
      began_ = false;
      return;
    }
    VkRenderPassBeginInfo begin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = render_pass;
    begin.framebuffer = framebuffer;
    begin.renderArea = pass.area;
    device_->functions().vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    began_ = true;
  }

  void End(VkCommandBuffer cmd) override {
    if (began_) device_->functions().vkCmdEndRenderPass(cmd);
    began_ = false;
  }

  void BeginSlot(uint32_t slot) override {
    current_slot_ = slot % uint32_t(slots_.size());
    DestroyFramebuffers(slots_[current_slot_]);
  }

  void Shutdown() override {
    if (!device_) return;
    for (auto& slot : slots_) DestroyFramebuffers(slot);
    const auto& dfn = device_->functions();
    for (auto& [key, pass] : render_passes_) {
      if (pass) dfn.vkDestroyRenderPass(device_->device(), pass, nullptr);
    }
    render_passes_.clear();
  }

 private:
  // Formats, samples and layouts of every attachment.
  using PassKey = std::array<uint32_t, 2 + 2 * (kMaxPassColors + 1)>;
  // Render pass, the views and the size.
  using FramebufferKey = std::array<uint64_t, 1 + kMaxPassColors + 1 + 1>;
  using SlotFramebuffers = std::map<FramebufferKey, VkFramebuffer>;

  VkRenderPass GetRenderPass(const PassFormats& formats, const PassAttachment* colors,
                             VkImageLayout depth_layout) {
    PassKey key = {};
    key[0] = formats.color_count;
    key[1] = uint32_t(formats.samples);
    for (uint32_t i = 0; i < formats.color_count; ++i) {
      key[2 + 2 * i] = uint32_t(formats.color[i]);
      key[3 + 2 * i] = formats.color[i] ? uint32_t(colors[i].layout) : 0;
    }
    key[2 + 2 * kMaxPassColors] = uint32_t(formats.depth_stencil);
    key[3 + 2 * kMaxPassColors] = formats.depth_stencil ? uint32_t(depth_layout) : 0;
    // Pipelines are created on worker threads too (MANTRA_ASYNC_PIPELINES).
    std::lock_guard<std::mutex> lock(render_passes_mutex_);
    if (auto it = render_passes_.find(key); it != render_passes_.end()) return it->second;

    VkAttachmentDescription attachments[kMaxPassColors + 1] = {};
    VkAttachmentReference color_refs[kMaxPassColors] = {};
    VkAttachmentReference depth_ref = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
    uint32_t count = 0;
    for (uint32_t i = 0; i < formats.color_count; ++i) {
      if (!formats.color[i]) {
        color_refs[i] = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
        continue;
      }
      VkAttachmentDescription& a = attachments[count];
      a.format = formats.color[i];
      a.samples = formats.samples;
      a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = a.finalLayout = colors[i].layout;
      color_refs[i] = {count++, colors[i].layout};
    }
    if (formats.depth_stencil) {
      VkAttachmentDescription& a = attachments[count];
      const bool stencil = HasStencil(formats.depth_stencil);
      a.format = formats.depth_stencil;
      a.samples = formats.samples;
      a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = stencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      a.stencilStoreOp = stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = a.finalLayout = depth_layout;
      depth_ref = {count++, depth_layout};
    }
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = formats.color_count;
    subpass.pColorAttachments = color_refs;
    subpass.pDepthStencilAttachment = formats.depth_stencil ? &depth_ref : nullptr;
    VkRenderPassCreateInfo info = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (device_->functions().vkCreateRenderPass(device_->device(), &info, nullptr, &pass) !=
        VK_SUCCESS) {
      MANTRA_ERROR("rendering: vkCreateRenderPass failed ({} colors, depth format {})",
                   formats.color_count, int32_t(formats.depth_stencil));
      pass = VK_NULL_HANDLE;
    }
    render_passes_.emplace(key, pass);  // failures too, reported once
    return pass;
  }

  VkFramebuffer GetFramebuffer(VkRenderPass render_pass, const PassFormats& formats,
                               const PassBegin& pass) {
    FramebufferKey key = {};
    key[0] = uint64_t(render_pass);
    VkImageView views[kMaxPassColors + 1];
    uint32_t count = 0;
    for (uint32_t i = 0; i < formats.color_count; ++i) {
      if (!formats.color[i]) continue;
      views[count++] = pass.color[i].view;
      key[1 + i] = uint64_t(pass.color[i].view);
    }
    if (formats.depth_stencil) {
      views[count++] = pass.depth_stencil.view;
      key[1 + kMaxPassColors] = uint64_t(pass.depth_stencil.view);
    }
    key[2 + kMaxPassColors] = (uint64_t(pass.extent.width) << 32) | pass.extent.height;
    SlotFramebuffers& slot = slots_[current_slot_];
    if (auto it = slot.find(key); it != slot.end()) return it->second;
    VkFramebufferCreateInfo info = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = render_pass;
    info.attachmentCount = count;
    info.pAttachments = views;
    info.width = pass.extent.width;
    info.height = pass.extent.height;
    info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (device_->functions().vkCreateFramebuffer(device_->device(), &info, nullptr,
                                                 &framebuffer) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    slot.emplace(key, framebuffer);
    return framebuffer;
  }

  void DestroyFramebuffers(SlotFramebuffers& slot) {
    const auto& dfn = device_->functions();
    for (auto& [key, framebuffer] : slot) {
      dfn.vkDestroyFramebuffer(device_->device(), framebuffer, nullptr);
    }
    slot.clear();
  }

  const rex::ui::vulkan::VulkanDevice* device_;
  std::mutex render_passes_mutex_;
  std::map<PassKey, VkRenderPass> render_passes_;
  std::vector<SlotFramebuffers> slots_;
  uint32_t current_slot_ = 0;
  bool began_ = false;
};

}  // namespace

std::unique_ptr<RenderingBackend> CreateRenderPassBackend(
    const rex::ui::vulkan::VulkanDevice* device, uint32_t slot_count) {
  return std::make_unique<RenderPassBackend>(device, slot_count);
}

}  // namespace mantra
