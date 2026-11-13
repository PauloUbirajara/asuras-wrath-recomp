#include "shader_interface.h"
#include "mantra_log.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace mantra {

namespace util = rex::ui::vulkan::util;

namespace {
constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
// Ring allocations are aligned like float4 constants.
constexpr VkDeviceSize kUploadAlignment = 16;
}  // namespace

bool ShaderInterface::Initialize(const rex::ui::vulkan::VulkanDevice* device,
                                 uint32_t frame_slots) {
  device_ = device;
  const auto& props = device->properties();
  // Each stage sees 3 * kFetchSlots sampled images and kFetchSlots samplers.
  if (props.maxPerStageDescriptorSampledImages < 3 * kFetchSlots ||
      props.maxPerStageDescriptorSamplers < kFetchSlots ||
      props.maxStorageBufferRange < kRingBytesPerSlot * frame_slots) {
    MANTRA_ERROR("Vulkan device limits too low for the shader interface (sampled images {}, "
                 "samplers {}, storage buffer range {})",
                 props.maxPerStageDescriptorSampledImages, props.maxPerStageDescriptorSamplers,
                 props.maxStorageBufferRange);
    return false;
  }
  texture_pools_.assign(frame_slots, VK_NULL_HANDLE);
  return CreateLayouts() && CreateRing(frame_slots) && CreatePlaceholders() &&
         CreateDescriptorSets();
}

bool ShaderInterface::CreateLayouts() {
  const auto& dfn = device_->functions();
  const VkDevice vk_device = device_->device();

  VkDescriptorSetLayoutBinding constants_binding = {};
  constants_binding.binding = 0;
  constants_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  constants_binding.descriptorCount = 1;
  constants_binding.stageFlags = kStages;
  VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &constants_binding;
  if (dfn.vkCreateDescriptorSetLayout(vk_device, &set_info, nullptr, &constants_layout_) !=
      VK_SUCCESS) {
    MANTRA_ERROR("failed to create the constants descriptor set layout");
    return false;
  }

  VkDescriptorSetLayoutBinding texture_bindings[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    texture_bindings[i].binding = i;
    texture_bindings[i].descriptorType =
        i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
    texture_bindings[i].descriptorCount = kFetchSlots;
    texture_bindings[i].stageFlags = kStages;
  }
  set_info.bindingCount = 4;
  set_info.pBindings = texture_bindings;
  if (dfn.vkCreateDescriptorSetLayout(vk_device, &set_info, nullptr, &textures_layout_) !=
      VK_SUCCESS) {
    MANTRA_ERROR("failed to create the textures descriptor set layout");
    return false;
  }

  const VkDescriptorSetLayout set_layouts[] = {constants_layout_, textures_layout_};
  VkPushConstantRange push_range = {};
  push_range.stageFlags = kStages;
  push_range.size = sizeof(DrawConstantOffsets);
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 2;
  layout_info.pSetLayouts = set_layouts;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (dfn.vkCreatePipelineLayout(vk_device, &layout_info, nullptr, &pipeline_layout_) !=
      VK_SUCCESS) {
    MANTRA_ERROR("failed to create the guest shader pipeline layout");
    return false;
  }
  return true;
}

bool ShaderInterface::CreateRing(uint32_t frame_slots) {
  const auto& dfn = device_->functions();
  if (!util::CreateDedicatedAllocationBuffer(
          device_, kRingBytesPerSlot * frame_slots,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          util::MemoryPurpose::kUpload, ring_buffer_, ring_memory_, &ring_memory_type_,
          &ring_memory_size_)) {
    MANTRA_ERROR("failed to create the constant ring ({} MB)",
                 (kRingBytesPerSlot * frame_slots) >> 20);
    return false;
  }
  void* mapping = nullptr;
  if (dfn.vkMapMemory(device_->device(), ring_memory_, 0, VK_WHOLE_SIZE, 0, &mapping) !=
      VK_SUCCESS) {
    MANTRA_ERROR("failed to map the constant ring");
    return false;
  }
  ring_mapping_ = static_cast<uint8_t*>(mapping);
  return true;
}

bool ShaderInterface::CreatePlaceholders() {
  const auto& dfn = device_->functions();
  const VkDevice vk_device = device_->device();
  for (uint32_t i = 0; i < 3; ++i) {
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = i == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = i == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = i == 2 ? 6 : 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!util::CreateDedicatedAllocationImage(device_, info, util::MemoryPurpose::kDeviceLocal,
                                              placeholder_images_[i], placeholder_memory_[i])) {
      MANTRA_ERROR("failed to create placeholder texture {}", i);
      return false;
    }
    VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = placeholder_images_[i];
    view_info.viewType = i == 0   ? VK_IMAGE_VIEW_TYPE_2D
                         : i == 1 ? VK_IMAGE_VIEW_TYPE_3D
                                  : VK_IMAGE_VIEW_TYPE_CUBE;
    view_info.format = info.format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, info.arrayLayers};
    if (dfn.vkCreateImageView(vk_device, &view_info, nullptr, &placeholder_views_[i]) !=
        VK_SUCCESS) {
      MANTRA_ERROR("failed to create placeholder texture view {}", i);
      return false;
    }
  }
  VkSamplerCreateInfo sampler_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  if (dfn.vkCreateSampler(vk_device, &sampler_info, nullptr, &placeholder_sampler_) !=
      VK_SUCCESS) {
    MANTRA_ERROR("failed to create the placeholder sampler");
    return false;
  }
  return true;
}

bool ShaderInterface::CreateDescriptorSets() {
  const auto& dfn = device_->functions();
  const VkDevice vk_device = device_->device();

  const VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kFetchSlots},
      {VK_DESCRIPTOR_TYPE_SAMPLER, kFetchSlots},
  };
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 2;
  pool_info.poolSizeCount = uint32_t(std::size(pool_sizes));
  pool_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(vk_device, &pool_info, nullptr, &descriptor_pool_) !=
      VK_SUCCESS) {
    MANTRA_ERROR("failed to create the shader interface descriptor pool");
    return false;
  }
  const VkDescriptorSetLayout layouts[] = {constants_layout_, textures_layout_};
  VkDescriptorSet sets[2] = {};
  VkDescriptorSetAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = descriptor_pool_;
  alloc_info.descriptorSetCount = 2;
  alloc_info.pSetLayouts = layouts;
  if (dfn.vkAllocateDescriptorSets(vk_device, &alloc_info, sets) != VK_SUCCESS) {
    MANTRA_ERROR("failed to allocate the shader interface descriptor sets");
    return false;
  }
  constants_set_ = sets[0];
  textures_set_ = sets[1];

  const VkDescriptorPoolSize texture_pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kFetchSlots * kTextureSetsPerSlot},
      {VK_DESCRIPTOR_TYPE_SAMPLER, kFetchSlots * kTextureSetsPerSlot},
  };
  VkDescriptorPoolCreateInfo texture_pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  texture_pool_info.maxSets = kTextureSetsPerSlot;
  texture_pool_info.poolSizeCount = uint32_t(std::size(texture_pool_sizes));
  texture_pool_info.pPoolSizes = texture_pool_sizes;
  for (VkDescriptorPool& pool : texture_pools_) {
    if (dfn.vkCreateDescriptorPool(vk_device, &texture_pool_info, nullptr, &pool) != VK_SUCCESS) {
      MANTRA_ERROR("failed to create a texture descriptor pool");
      return false;
    }
  }

  VkDescriptorBufferInfo buffer_info = {ring_buffer_, 0, VK_WHOLE_SIZE};
  VkDescriptorImageInfo image_infos[3][kFetchSlots] = {};
  VkDescriptorImageInfo sampler_infos[kFetchSlots] = {};
  for (uint32_t d = 0; d < 3; ++d) {
    for (uint32_t s = 0; s < kFetchSlots; ++s) {
      image_infos[d][s].imageView = placeholder_views_[d];
      image_infos[d][s].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
  }
  for (uint32_t s = 0; s < kFetchSlots; ++s) {
    sampler_infos[s].sampler = placeholder_sampler_;
  }
  VkWriteDescriptorSet writes[5] = {};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = constants_set_;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &buffer_info;
  for (uint32_t i = 0; i < 4; ++i) {
    VkWriteDescriptorSet& w = writes[1 + i];
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = textures_set_;
    w.dstBinding = i;
    w.descriptorCount = kFetchSlots;
    w.descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = i < 3 ? image_infos[i] : sampler_infos;
  }
  dfn.vkUpdateDescriptorSets(vk_device, uint32_t(std::size(writes)), writes, 0, nullptr);
  return true;
}

void ShaderInterface::InitializeResources(VkCommandBuffer cmd) {
  if (placeholders_initialized_) {
    return;
  }
  placeholders_initialized_ = true;
  const auto& dfn = device_->functions();
  VkImageMemoryBarrier barriers[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[i].image = placeholder_images_[i];
    barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, i == 2 ? 6u : 1u};
  }
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 3,
                           barriers);
  // Transparent black, like an unbound fetch on the guest.
  const VkClearColorValue white = {{0.0f, 0.0f, 0.0f, 0.0f}};
  for (uint32_t i = 0; i < 3; ++i) {
    dfn.vkCmdClearColorImage(cmd, placeholder_images_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &white, 1, &barriers[i].subresourceRange);
    barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 3, barriers);
}

void ShaderInterface::Shutdown() {
  if (!device_) {
    return;
  }
  const auto& dfn = device_->functions();
  const VkDevice vk_device = device_->device();
  if (descriptor_pool_) dfn.vkDestroyDescriptorPool(vk_device, descriptor_pool_, nullptr);
  for (VkDescriptorPool pool : texture_pools_) {
    if (pool) dfn.vkDestroyDescriptorPool(vk_device, pool, nullptr);
  }
  if (placeholder_sampler_) dfn.vkDestroySampler(vk_device, placeholder_sampler_, nullptr);
  for (uint32_t i = 0; i < 3; ++i) {
    if (placeholder_views_[i]) dfn.vkDestroyImageView(vk_device, placeholder_views_[i], nullptr);
    if (placeholder_images_[i]) dfn.vkDestroyImage(vk_device, placeholder_images_[i], nullptr);
    if (placeholder_memory_[i]) dfn.vkFreeMemory(vk_device, placeholder_memory_[i], nullptr);
  }
  if (ring_mapping_) dfn.vkUnmapMemory(vk_device, ring_memory_);
  if (ring_buffer_) dfn.vkDestroyBuffer(vk_device, ring_buffer_, nullptr);
  if (ring_memory_) dfn.vkFreeMemory(vk_device, ring_memory_, nullptr);
  if (pipeline_layout_) dfn.vkDestroyPipelineLayout(vk_device, pipeline_layout_, nullptr);
  if (textures_layout_) dfn.vkDestroyDescriptorSetLayout(vk_device, textures_layout_, nullptr);
  if (constants_layout_) dfn.vkDestroyDescriptorSetLayout(vk_device, constants_layout_, nullptr);
  *this = ShaderInterface();
}

void ShaderInterface::BeginSlot(uint32_t slot) {
  current_slot_ = slot;
  if (slot < texture_pools_.size() && texture_pools_[slot]) {
    device_->functions().vkResetDescriptorPool(device_->device(), texture_pools_[slot], 0);
  }
  slot_begin_ = kRingBytesPerSlot * slot;
  cursor_ = slot_begin_;
  flushed_ = slot_begin_;
}

void ShaderInterface::FlushSlot() {
  if (cursor_ > flushed_) {
    util::FlushMappedMemoryRange(device_, ring_memory_, ring_memory_type_, flushed_,
                                 ring_memory_size_, cursor_ - flushed_);
    flushed_ = cursor_;
  }
}

uint8_t* ShaderInterface::Allocate(uint32_t size, uint32_t& offset) {
  const VkDeviceSize at = (cursor_ + kUploadAlignment - 1) & ~(kUploadAlignment - 1);
  if (at + size > slot_begin_ + kRingBytesPerSlot) {
    return nullptr;
  }
  offset = uint32_t(at);
  cursor_ = at + size;
  return ring_mapping_ + at;
}

bool ShaderInterface::Upload(const void* data, uint32_t size, uint32_t& offset) {
  uint8_t* dest = Allocate(size, offset);
  if (!dest) {
    return false;
  }
  std::memcpy(dest, data, size);
  return true;
}

VkDescriptorSet ShaderInterface::AllocateTextureSet(const VkImageView views[3][kFetchSlots],
                                                   const VkSampler samplers[kFetchSlots]) {
  const auto& dfn = device_->functions();
  const VkDevice vk_device = device_->device();
  VkDescriptorSetAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = texture_pools_[current_slot_];
  alloc_info.descriptorSetCount = 1;
  alloc_info.pSetLayouts = &textures_layout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (dfn.vkAllocateDescriptorSets(vk_device, &alloc_info, &set) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  VkDescriptorImageInfo image_infos[3][kFetchSlots] = {};
  VkDescriptorImageInfo sampler_infos[kFetchSlots] = {};
  for (uint32_t d = 0; d < 3; ++d) {
    for (uint32_t i = 0; i < kFetchSlots; ++i) {
      image_infos[d][i].imageView = views[d][i] ? views[d][i] : placeholder_views_[d];
      image_infos[d][i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
  }
  for (uint32_t i = 0; i < kFetchSlots; ++i) {
    sampler_infos[i].sampler = samplers[i] ? samplers[i] : placeholder_sampler_;
  }
  VkWriteDescriptorSet writes[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    VkWriteDescriptorSet& w = writes[i];
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = i;
    w.descriptorCount = kFetchSlots;
    w.descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = i < 3 ? image_infos[i] : sampler_infos;
  }
  dfn.vkUpdateDescriptorSets(vk_device, 4, writes, 0, nullptr);
  return set;
}

void ShaderInterface::Bind(VkCommandBuffer cmd, const DrawConstantOffsets& offsets,
                           VkDescriptorSet textures) const {
  const auto& dfn = device_->functions();
  const VkDescriptorSet sets[] = {constants_set_, textures ? textures : textures_set_};
  dfn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 2, sets,
                              0, nullptr);
  dfn.vkCmdPushConstants(cmd, pipeline_layout_, kStages, 0, sizeof(offsets), &offsets);
}

VkShaderModule ShaderInterface::CreateModule(std::span<const uint8_t> spirv) const {
  if (spirv.empty() || spirv.size() % 4) {
    return VK_NULL_HANDLE;
  }
  // The cache blob may not be 4-byte aligned in memory; copy it.
  std::vector<uint32_t> code(spirv.size() / 4);
  std::memcpy(code.data(), spirv.data(), spirv.size());
  return util::CreateShaderModule(device_, code.data(), spirv.size());
}

}  // namespace mantra
