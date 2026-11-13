#include "draw_pipelines.h"
#include "mantra_log.h"

#include <atomic>

#include <xxhash.h>

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace mantra {

namespace {

// SPIR-V compiled at build time by glslc (see CMakeLists.txt).
const uint32_t kRectListGs[] = {
#include "rect_list.geom.inc"
};

}  // namespace

size_t DrawPipelines::KeyHash::operator()(const PipelineKey& key) const {
  return size_t(XXH3_64bits(&key, sizeof(key)));
}

bool DrawPipelines::Initialize(const rex::ui::vulkan::VulkanDevice* device,
                               VkPipelineLayout layout) {
  device_ = device;
  layout_ = layout;
  if (device->properties().geometryShader) {
    rect_list_gs_ =
        rex::ui::vulkan::util::CreateShaderModule(device, kRectListGs, sizeof(kRectListGs));
  }
  if (!rect_list_gs_) {
    MANTRA_WARN("no geometry shaders: rectangle-list draws will be skipped");
  }
  return true;
}

void DrawPipelines::Shutdown() {
  if (!device_) {
    return;
  }
  const auto& dfn = device_->functions();
  const VkDevice device = device_->device();
  for (auto& [key, pipeline] : pipelines_) {
    if (pipeline) dfn.vkDestroyPipeline(device, pipeline, nullptr);
  }
  pipelines_.clear();
  if (rect_list_gs_) {
    dfn.vkDestroyShaderModule(device, rect_list_gs_, nullptr);
    rect_list_gs_ = VK_NULL_HANDLE;
  }
  device_ = nullptr;
}

VkPipeline DrawPipelines::Get(const PipelineKey& key) {
  if (auto it = pipelines_.find(key); it != pipelines_.end()) {
    return it->second;
  }
  VkPipeline pipeline = Create(key);
  if (!pipeline) {
    ++creation_failures_;
  }
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

VkPipeline DrawPipelines::Create(const PipelineKey& key) {
  const auto& dfn = device_->functions();

  VkSpecializationMapEntry spec_entry = {0, 0, sizeof(uint32_t)};
  VkSpecializationInfo spec_info = {1, &spec_entry, sizeof(key.spec_constants),
                                    &key.spec_constants};
  VkPipelineShaderStageCreateInfo stages[3] = {};
  uint32_t stage_count = 0;
  auto add_stage = [&](VkShaderStageFlagBits stage, uint64_t module, bool specialized) {
    VkPipelineShaderStageCreateInfo& s = stages[stage_count++];
    s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    s.stage = stage;
    s.module = VkShaderModule(module);
    s.pName = "main";
    s.pSpecializationInfo = specialized ? &spec_info : nullptr;
  };
  add_stage(VK_SHADER_STAGE_VERTEX_BIT, key.vertex_module, true);
  if (key.rect_list) {
    if (!rect_list_gs_) {
      return VK_NULL_HANDLE;
    }
    add_stage(VK_SHADER_STAGE_GEOMETRY_BIT, uint64_t(rect_list_gs_), false);
  }
  if (key.pixel_module) {
    add_stage(VK_SHADER_STAGE_FRAGMENT_BIT, key.pixel_module, true);
  }

  VkVertexInputBindingDescription bindings[kMaxVertexBindings];
  for (uint32_t i = 0; i < key.binding_count; ++i) {
    bindings[i] = {i, key.binding_strides[i], VK_VERTEX_INPUT_RATE_VERTEX};
  }
  VkVertexInputAttributeDescription attributes[kMaxVertexAttributes];
  for (uint32_t i = 0; i < key.attribute_count; ++i) {
    const auto& a = key.attributes[i];
    attributes[i] = {a.location, a.binding, VkFormat(a.format), a.offset};
  }
  VkPipelineVertexInputStateCreateInfo vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = key.binding_count;
  vertex_input.pVertexBindingDescriptions = bindings;
  vertex_input.vertexAttributeDescriptionCount = key.attribute_count;
  vertex_input.pVertexAttributeDescriptions = attributes;

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VkPrimitiveTopology(key.topology);
  input_assembly.primitiveRestartEnable = key.primitive_restart;

  VkPipelineViewportStateCreateInfo viewport = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VkPolygonMode(key.polygon_mode);
  raster.cullMode = key.cull_mode;
  raster.frontFace = VkFrontFace(key.front_face);
  raster.depthBiasEnable = key.depth_bias;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VkSampleCountFlagBits(key.sample_count);
  multisample.alphaToCoverageEnable = key.alpha_to_coverage;

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth_stencil.depthTestEnable = key.depth_test;
  depth_stencil.depthWriteEnable = key.depth_write;
  depth_stencil.depthCompareOp = VkCompareOp(key.depth_compare);
  depth_stencil.stencilTestEnable = key.stencil_test;
  auto stencil = [](const uint8_t ops[4]) {
    VkStencilOpState s = {};
    s.failOp = VkStencilOp(ops[0]);
    s.passOp = VkStencilOp(ops[1]);
    s.depthFailOp = VkStencilOp(ops[2]);
    s.compareOp = VkCompareOp(ops[3]);
    return s;
  };
  depth_stencil.front = stencil(key.stencil_front);
  depth_stencil.back = stencil(key.stencil_back);

  VkPipelineColorBlendAttachmentState blend[kMaxColorAttachments] = {};
  for (uint32_t i = 0; i < key.color_count; ++i) {
    const auto& b = key.blend[i];
    blend[i].blendEnable = b.enable;
    blend[i].srcColorBlendFactor = VkBlendFactor(b.src_color);
    blend[i].dstColorBlendFactor = VkBlendFactor(b.dst_color);
    blend[i].colorBlendOp = VkBlendOp(b.color_op);
    blend[i].srcAlphaBlendFactor = VkBlendFactor(b.src_alpha);
    blend[i].dstAlphaBlendFactor = VkBlendFactor(b.dst_alpha);
    blend[i].alphaBlendOp = VkBlendOp(b.alpha_op);
    blend[i].colorWriteMask = b.write_mask;
  }
  VkPipelineColorBlendStateCreateInfo color_blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color_blend.attachmentCount = key.color_count;
  color_blend.pAttachments = blend;

  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,         VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,
  };
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;

  VkFormat color_formats[kMaxColorAttachments];
  for (uint32_t i = 0; i < key.color_count; ++i) {
    color_formats[i] = VkFormat(key.color_formats[i]);
  }
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = key.color_count;
  rendering.pColorAttachmentFormats = color_formats;
  rendering.depthAttachmentFormat = VkFormat(key.depth_format);
  rendering.stencilAttachmentFormat = VkFormat(key.depth_format);

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth_stencil;
  info.pColorBlendState = &color_blend;
  info.pDynamicState = &dynamic;
  info.layout = layout_;

  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult result =
      dfn.vkCreateGraphicsPipelines(device_->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
  if (result != VK_SUCCESS) {
    MANTRA_WARN("vkCreateGraphicsPipelines failed ({}) for VS {:X} PS {:X}", int32_t(result),
                key.vertex_module, key.pixel_module);
    return VK_NULL_HANDLE;
  }
  MANTRA_DEBUG("pipeline {}: {} attributes, {} color, depth {}, topology {}{}", pipelines_.size(),
               key.attribute_count, key.color_count, key.depth_format, key.topology,
               key.rect_list ? " (rect list)" : "");
  return pipeline;
}

}  // namespace mantra
