#include "copy_pipelines.h"

#include <cmath>
#include "mantra_log.h"

#include <atomic>

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace mantra {

namespace {

// SPIR-V compiled at build time by glslc (see CMakeLists.txt).
const uint32_t kFullscreenVs[] = {
#include "fullscreen.vert.inc"
};
const uint32_t kCopyFs[] = {
#include "copy.frag.inc"
};
const uint32_t kCopyMsFs[] = {
#include "copy_ms.frag.inc"
};

}  // namespace

bool CopyPipelines::Initialize(const rex::ui::vulkan::VulkanDevice* device) {
  device_ = device;
  const auto& dfn = device->functions();
  const VkDevice vk_device = device->device();

  if (!device->properties().dynamicRendering) {
    MANTRA_ERROR("the Vulkan device has no dynamic rendering (Vulkan 1.3 or "
                 "VK_KHR_dynamic_rendering is required)");
    return false;
  }

  vs_ = rex::ui::vulkan::util::CreateShaderModule(device, kFullscreenVs, sizeof(kFullscreenVs));
  fs_ = rex::ui::vulkan::util::CreateShaderModule(device, kCopyFs, sizeof(kCopyFs));
  fs_ms_ = rex::ui::vulkan::util::CreateShaderModule(device, kCopyMsFs, sizeof(kCopyMsFs));
  if (!vs_ || !fs_ || !fs_ms_) {
    MANTRA_ERROR("failed to create the copy shader modules");
    return false;
  }

  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo set_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  if (dfn.vkCreateDescriptorSetLayout(vk_device, &set_info, nullptr, &set_layout_) !=
      VK_SUCCESS) {
    MANTRA_ERROR("failed to create the copy descriptor set layout");
    return false;
  }

  VkPushConstantRange push_range = {};
  push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  push_range.size = sizeof(CopyPushConstants);
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (dfn.vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &pipeline_layout_) !=
      VK_SUCCESS) {
    MANTRA_ERROR("failed to create the copy pipeline layout");
    return false;
  }
  return true;
}

void CopyPipelines::Shutdown() {
  if (!device_) {
    return;
  }
  const auto& dfn = device_->functions();
  const VkDevice vk_device = device_->device();
  for (auto& [key, pipeline] : pipelines_) {
    dfn.vkDestroyPipeline(vk_device, pipeline, nullptr);
  }
  pipelines_.clear();
  if (pipeline_layout_) dfn.vkDestroyPipelineLayout(vk_device, pipeline_layout_, nullptr);
  if (set_layout_) dfn.vkDestroyDescriptorSetLayout(vk_device, set_layout_, nullptr);
  if (vs_) dfn.vkDestroyShaderModule(vk_device, vs_, nullptr);
  if (fs_) dfn.vkDestroyShaderModule(vk_device, fs_, nullptr);
  if (fs_ms_) dfn.vkDestroyShaderModule(vk_device, fs_ms_, nullptr);
  pipeline_layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
  vs_ = fs_ = fs_ms_ = VK_NULL_HANDLE;
  device_ = nullptr;
}

VkPipeline CopyPipelines::GetPipeline(VkFormat dst_format, bool src_multisampled) {
  const uint64_t key = (uint64_t(dst_format) << 1) | uint64_t(src_multisampled);
  if (auto it = pipelines_.find(key); it != pipelines_.end()) {
    return it->second;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs_;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = src_multisampled ? fs_ms_ : fs_;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend_attachment = {};
  blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;
  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2;
  dynamic.pDynamicStates = dynamic_states;

  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &dst_format;

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = pipeline_layout_;

  VkPipeline pipeline = VK_NULL_HANDLE;
  if (device_->functions().vkCreateGraphicsPipelines(device_->device(), VK_NULL_HANDLE, 1, &info,
                                                     nullptr, &pipeline) != VK_SUCCESS) {
    MANTRA_ERROR("failed to create a copy pipeline for format {}{}", int32_t(dst_format),
                 src_multisampled ? " (multisampled source)" : "");
    pipeline = VK_NULL_HANDLE;
  }
  // Cache failures too, so they are reported once.
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool CopyPipelines::Record(VkCommandBuffer cmd, VkDescriptorPool descriptor_pool,
                           VkImageView src_view, bool src_multisampled, VkImageView dst_view,
                           VkFormat dst_format, uint32_t dst_width, uint32_t dst_height,
                           const CopyRect& guest_rect, const CopyPushConstants& guest_constants,
                           uint32_t rscale, uint32_t src_rscale, bool linear) {
  if (!src_rscale) src_rscale = rscale;
  const VkRect2D host = HostRect(
      {{guest_rect.x, guest_rect.y}, {guest_rect.width, guest_rect.height}}, rscale);
  const CopyRect rect = {host.offset.x, host.offset.y, host.extent.width, host.extent.height};
  dst_width = HostSize(dst_width, rscale);
  dst_height = HostSize(dst_height, rscale);
  CopyPushConstants constants = guest_constants;
  for (int32_t& offset : constants.src_offset) {
    offset = int32_t(std::lround(double(offset) * src_rscale / double(kScaleOne)));
  }
  constants.src_ratio = float(src_rscale) / float(rscale);
  constants.linear = linear && !src_multisampled && src_rscale != rscale ? 1u : 0u;
  if (!rect.width || !rect.height) {
    return true;
  }
  VkPipeline pipeline = GetPipeline(dst_format, src_multisampled);
  if (!pipeline) {
    return false;
  }
  const auto& dfn = device_->functions();
  const VkDevice vk_device = device_->device();

  VkDescriptorSetAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = descriptor_pool;
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &set_layout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (dfn.vkAllocateDescriptorSets(vk_device, &alloc_info, &set) != VK_SUCCESS) {
    MANTRA_WARN_ONCE("copy descriptor pool exhausted, dropping copies this frame");
    return false;
  }
  VkDescriptorImageInfo image_info = {};
  image_info.imageView = src_view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &image_info;
  dfn.vkUpdateDescriptorSets(vk_device, 1, &write, 0, nullptr);

  VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = dst_view;
  attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.offset = {rect.x, rect.y};
  rendering.renderArea.extent = {rect.width, rect.height};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &attachment;
  dfn.vkCmdBeginRendering(cmd, &rendering);

  // The viewport spans the whole destination so gl_FragCoord is in
  // destination pixels; the scissor picks the rectangle.
  VkViewport vp = {0.0f, 0.0f, float(dst_width), float(dst_height), 0.0f, 1.0f};
  dfn.vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D scissor = rendering.renderArea;
  dfn.vkCmdSetScissor(cmd, 0, 1, &scissor);
  dfn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set,
                              0, nullptr);
  dfn.vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(constants), &constants);
  dfn.vkCmdDraw(cmd, 3, 1, 0, 0);
  dfn.vkCmdEndRendering(cmd);
  return true;
}

}  // namespace mantra
