#include "texture_cache.h"
#include "buffer_cache.h"
#include "mantra_log.h"
#include "render_targets.h"

#include <algorithm>

#include <xxhash.h>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/math.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

namespace mantra {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;
namespace util = rex::ui::vulkan::util;
using rex::graphics::FormatInfo;

namespace {

constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;
constexpr VkPipelineStageFlags kShaderStages =
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

struct HostFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t component_count = 4;
  // Host channel of guest components X, Y, Z, W.
  VkComponentSwizzle channels[4] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                    VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
};

// Guest format -> host format. Xenos keeps component X in the lowest bits of
// packed formats (after the endian swap), so the host format is the one whose
// first channel sits there, permuted through `channels` where Vulkan has no
// matching order.
HostFormat GetHostFormat(const xenos::xe_gpu_texture_fetch_t& fetch) {
  using F = xenos::TextureFormat;
  using S = xenos::TextureSign;
  HostFormat h;
  const bool all_signed = fetch.sign_x == S::kSigned && fetch.sign_y == S::kSigned &&
                          fetch.sign_z == S::kSigned && fetch.sign_w == S::kSigned;
  const bool x_signed = fetch.sign_x == S::kSigned;
  const bool gamma = fetch.sign_x == S::kGamma;
  auto pick = [](bool s, VkFormat u, VkFormat sn) { return s ? sn : u; };
  switch (rex::graphics::GetBaseFormat(fetch.format)) {
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
      h.format = pick(x_signed, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM);
      h.component_count = 1;
      break;
    case F::k_8_8:
      h.format = pick(fetch.sign_x == S::kSigned && fetch.sign_y == S::kSigned,
                      VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM);
      h.component_count = 2;
      break;
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
      h.format = gamma ? VK_FORMAT_R8G8B8A8_SRGB
                       : pick(all_signed, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM);
      break;
    case F::k_1_5_5_5:
      // X in bits 0..4: B of A1R5G5B5.
      h.format = VK_FORMAT_A1R5G5B5_UNORM_PACK16;
      h.channels[0] = VK_COMPONENT_SWIZZLE_B;
      h.channels[2] = VK_COMPONENT_SWIZZLE_R;
      break;
    case F::k_5_6_5:
      // X in bits 0..4: R of B5G6R5.
      h.format = VK_FORMAT_B5G6R5_UNORM_PACK16;
      h.component_count = 3;
      break;
    case F::k_4_4_4_4:
      // X in bits 0..3: A of R4G4B4A4.
      h.format = VK_FORMAT_R4G4B4A4_UNORM_PACK16;
      h.channels[0] = VK_COMPONENT_SWIZZLE_A;
      h.channels[1] = VK_COMPONENT_SWIZZLE_B;
      h.channels[2] = VK_COMPONENT_SWIZZLE_G;
      h.channels[3] = VK_COMPONENT_SWIZZLE_R;
      break;
    case F::k_2_10_10_10:
      h.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      break;
    case F::k_DXT1:
      h.format = gamma ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
      break;
    case F::k_DXT2_3:
      h.format = gamma ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
      break;
    case F::k_DXT4_5:
      h.format = gamma ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
      break;
    case F::k_DXN:
      h.format = VK_FORMAT_BC5_UNORM_BLOCK;
      h.component_count = 2;
      break;
    case F::k_DXT5A:
      h.format = VK_FORMAT_BC4_UNORM_BLOCK;
      h.component_count = 1;
      break;
    case F::k_16:
      h.format = pick(x_signed, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM);
      h.component_count = 1;
      break;
    case F::k_16_16:
      h.format = pick(fetch.sign_x == S::kSigned && fetch.sign_y == S::kSigned,
                      VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM);
      h.component_count = 2;
      break;
    case F::k_16_16_16_16:
      h.format = pick(all_signed, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM);
      break;
    case F::k_16_FLOAT:
      h.format = VK_FORMAT_R16_SFLOAT;
      h.component_count = 1;
      break;
    case F::k_16_16_FLOAT:
      h.format = VK_FORMAT_R16G16_SFLOAT;
      h.component_count = 2;
      break;
    case F::k_16_16_16_16_FLOAT:
      h.format = VK_FORMAT_R16G16B16A16_SFLOAT;
      break;
    case F::k_32_FLOAT:
      h.format = VK_FORMAT_R32_SFLOAT;
      h.component_count = 1;
      break;
    case F::k_32_32_FLOAT:
      h.format = VK_FORMAT_R32G32_SFLOAT;
      h.component_count = 2;
      break;
    case F::k_32_32_32_32_FLOAT:
      h.format = VK_FORMAT_R32G32B32A32_SFLOAT;
      break;
    default:
      break;
  }
  return h;
}

VkSamplerAddressMode AddressMode(xenos::ClampMode mode, bool mirror_clamp_supported) {
  using C = xenos::ClampMode;
  switch (mode) {
    case C::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case C::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case C::kClampToEdge:
    case C::kClampToHalfway: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case C::kMirrorClampToEdge:
    case C::kMirrorClampToHalfway:
    case C::kMirrorClampToBorder:
      return mirror_clamp_supported ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                    : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case C::kClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  }
  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

// Guest component selected by a 3-bit swizzle value -> host component.
VkComponentSwizzle SwizzleComponent(uint32_t select, const uint8_t channels[4],
                                    uint32_t component_count) {
  if (select == xenos::XE_GPU_TEXTURE_SWIZZLE_0) return VK_COMPONENT_SWIZZLE_ZERO;
  if (select >= xenos::XE_GPU_TEXTURE_SWIZZLE_1) return VK_COMPONENT_SWIZZLE_ONE;
  // Components a format doesn't have repeat its last one.
  select = std::min(select, component_count - 1);
  return VkComponentSwizzle(channels[select]);
}

}  // namespace

size_t TextureCache::KeyHash::operator()(const Key& key) const {
  return size_t(XXH3_64bits(&key, sizeof(key)));
}

bool TextureCache::Initialize(const rex::ui::vulkan::VulkanDevice* device, BufferCache* watcher,
                              ResolvedSurfaces* resolved) {
  device_ = device;
  watcher_ = watcher;
  resolved_ = resolved;
  // Not in the SDK's device function table.
  cmd_copy_image_ = reinterpret_cast<PFN_vkCmdCopyImage>(
      device->vulkan_instance()->functions().vkGetDeviceProcAddr(device->device(),
                                                                 "vkCmdCopyImage"));
  return true;
}

void TextureCache::Shutdown() {
  if (!device_) {
    return;
  }
  const auto& dfn = device_->functions();
  const VkDevice device = device_->device();
  for (auto& [key, texture] : textures_) {
    for (auto& [swizzle, view] : texture->views) dfn.vkDestroyImageView(device, view, nullptr);
    if (texture->image) dfn.vkDestroyImage(device, texture->image, nullptr);
    if (texture->memory) dfn.vkFreeMemory(device, texture->memory, nullptr);
  }
  textures_.clear();
  for (auto& [key, sampler] : samplers_) dfn.vkDestroySampler(device, sampler, nullptr);
  samplers_.clear();
  resolves_.clear();
  device_ = nullptr;
}

// --- Lookup -----------------------------------------------------------------

TextureCache::Binding TextureCache::Get(const xenos::xe_gpu_texture_fetch_t& fetch, Host& host) {
  Binding binding;
  binding.dimension = fetch.dimension == xenos::DataDimension::kCube ? kCube
                      : fetch.dimension == xenos::DataDimension::k3D ? k3D
                                                                     : k2D;
  binding.sampler = GetSampler(fetch);

  // Data-describing parts of the fetch constant (no sampler state, swizzle).
  Key key;
  key.words[0] = fetch.dword_0 & ~((0x1FFu << 10) | 0x3u);  // drop clamp modes, type
  key.words[1] = fetch.dword_1;
  key.words[2] = fetch.dword_2;
  key.words[3] = 0;
  key.words[4] = fetch.dword_4 & (0xFFu << 2);  // mip min/max levels
  key.words[5] = fetch.dword_5 & ~0x1FFu;       // dimension, packed mips, mip address

  auto [it, inserted] = textures_.try_emplace(key);
  if (inserted) {
    it->second = std::make_unique<Texture>();
  }
  Texture& texture = *it->second;
  if (inserted) {
    const HostFormat host_format = GetHostFormat(fetch);
    texture.format = host_format.format;
    texture.component_count = host_format.component_count;
    for (uint32_t i = 0; i < 4; ++i) texture.channels[i] = uint8_t(host_format.channels[i]);
    texture.dimension = binding.dimension;
    if (texture.format == VK_FORMAT_UNDEFINED || binding.dimension == k3D) {
      texture.unsupported = true;
      MANTRA_DEBUG("texture format {} dimension {} not supported yet", uint32_t(fetch.format),
                   uint32_t(fetch.dimension));
    }
  }
  if (texture.unsupported) {
    ++stats_.unsupported;
    return binding;
  }

  const uint32_t base_address = (fetch.base_address << 12) & kPhysicalMask;
  bool ready = texture.initialized;
  if (base_address && ResolvedValid(base_address)) {
    ready = CopyFromResolved(texture, fetch, base_address, host);
    if (ready) {
      ++stats_.resolved_binds;
    }
  } else if (texture.resolved_image != VK_NULL_HANDLE || !texture.initialized ||
             !watcher_->RangeUnchanged(texture.base_start, texture.base_length, texture.epoch,
                                       texture.base_checked) ||
             !watcher_->RangeUnchanged(texture.mip_start, texture.mip_length, texture.epoch,
                                       texture.mip_checked)) {
    if (texture.initialized && texture.resolved_image == VK_NULL_HANDLE) {
      ++stats_.reloads;
    }
    ready = LoadFromMemory(texture, fetch, host);
  }
  if (!ready) {
    return binding;
  }
  binding.view = GetView(texture, fetch.swizzle);
  return binding;
}

// --- Loading ----------------------------------------------------------------

bool TextureCache::CreateImage(Texture& texture) {
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = texture.dimension == kCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = texture.format;
  info.extent = {texture.width, texture.height, 1};
  info.mipLevels = texture.levels;
  info.arrayLayers = texture.layers;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!util::CreateDedicatedAllocationImage(device_, info, util::MemoryPurpose::kDeviceLocal,
                                            texture.image, texture.memory)) {
    MANTRA_WARN_ONCE("failed to create a {}x{} texture (format {})", texture.width,
                     texture.height, int32_t(texture.format));
    return false;
  }
  return true;
}

bool TextureCache::LoadFromMemory(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch,
                                  Host& host) {
  const FormatInfo* format_info = FormatInfo::Get(fetch.format);
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page, &mip_min,
                                                 &mip_max);
  if (!format_info || (!base_page && !mip_page)) {
    ++stats_.failures;
    return false;
  }
  const uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1;
  const uint32_t array_size = depth_minus_1 + 1;
  const uint32_t layers = texture.dimension == kCube ? 6 : 1;  // stacked: layer 0 only
  const auto layout = texture_util::GetGuestTextureLayout(
      fetch.dimension, fetch.pitch, width, height, array_size, fetch.tiled, fetch.format,
      fetch.packed_mips, base_page != 0, mip_max);

  // Host levels are guest levels mip_min..mip_max.
  const uint32_t host_width = std::max(width >> mip_min, 1u);
  const uint32_t host_height = std::max(height >> mip_min, 1u);
  const uint32_t host_levels = mip_max - mip_min + 1;
  if (!texture.image) {
    texture.width = host_width;
    texture.height = host_height;
    texture.layers = layers;
    texture.levels = host_levels;
    if (!CreateImage(texture)) {
      texture.unsupported = true;
      ++stats_.failures;
      return false;
    }
  }

  // Watch first, then read: a racing CPU write makes the next use reload.
  const uint32_t base_address = base_page << 12;
  const uint32_t mip_address = mip_page << 12;
  const uint32_t base_length = base_page ? layout.base.level_data_extent_bytes : 0;
  const uint32_t mip_length = mip_page ? layout.mips_total_extent_bytes : 0;
  texture.base_start = base_address;
  texture.base_length = base_length;
  texture.mip_start = mip_address;
  texture.mip_length = mip_length;
  texture.epoch = watcher_->WatchRange(base_address, base_length);
  watcher_->WatchRange(mip_address, mip_length);
  texture.base_checked = texture.mip_checked = texture.epoch;

  // The endian swap applies to memory words, so the whole source range is
  // swapped before untiling.
  if (uint64_t(base_address) + base_length > uint64_t(kPhysicalMask) + 1 ||
      uint64_t(mip_address) + mip_length > uint64_t(kPhysicalMask) + 1) {
    ++stats_.failures;
    return false;
  }
  const uint8_t* base_src = watcher_->Translate(base_address);
  const uint8_t* mip_src = watcher_->Translate(mip_address);
  swapped_base_.assign((base_length + 3) & ~3u, 0);
  swapped_mips_.assign((mip_length + 3) & ~3u, 0);
  if ((base_length && !base_src) || (mip_length && !mip_src)) {
    ++stats_.failures;
    return false;
  }
  if (base_length) {
    rex::graphics::texture_conversion::CopySwapBlock(fetch.endianness, swapped_base_.data(),
                                                     base_src, base_length & ~3u);
  }
  if (mip_length) {
    rex::graphics::texture_conversion::CopySwapBlock(fetch.endianness, swapped_mips_.data(),
                                                     mip_src, mip_length & ~3u);
  }
  stats_.load_bytes += base_length + mip_length;

  // Staging: every level, all layers, tightly packed, 16-byte aligned.
  const uint32_t bw = format_info->block_width, bh = format_info->block_height;
  const uint32_t bpb = format_info->bytes_per_block();
  const uint32_t bpb_log2 = rex::log2_floor(bpb);
  uint32_t staging_size = 0;
  uint32_t level_offsets[xenos::kTextureMaxMips + 1];
  for (uint32_t h = 0; h < host_levels; ++h) {
    level_offsets[h] = staging_size;
    const uint32_t lw = std::max(host_width >> h, 1u), lh = std::max(host_height >> h, 1u);
    staging_size += ((lw + bw - 1) / bw) * ((lh + bh - 1) / bh) * bpb * layers;
    staging_size = (staging_size + 15) & ~15u;
  }
  VkBuffer staging_buffer;
  VkDeviceSize staging_offset;
  uint8_t* staging = host.AllocateStaging(staging_size, staging_buffer, staging_offset);
  if (!staging) {
    ++stats_.failures;
    return false;
  }
  std::memset(staging, 0, staging_size);

  VkBufferImageCopy regions[xenos::kTextureMaxMips + 1] = {};
  for (uint32_t h = 0; h < host_levels; ++h) {
    const uint32_t level = mip_min + h;
    const bool is_base = level == 0;
    const uint32_t stored_level =
        is_base ? 0 : std::min(level, layout.packed_level == UINT32_MAX ? level : layout.packed_level);
    const auto& level_layout = is_base ? layout.base : layout.mips[stored_level];
    const std::vector<uint8_t>& source = is_base ? swapped_base_ : swapped_mips_;
    const uint32_t source_offset = is_base ? 0 : layout.mip_offsets_bytes[stored_level];
    uint32_t packed_x = 0, packed_y = 0, packed_z = 0;
    if (layout.packed_level != UINT32_MAX && level >= layout.packed_level) {
      texture_util::GetPackedMipOffset(width, height, 1, fetch.format, level, packed_x, packed_y,
                                       packed_z);
    }
    const uint32_t lw = std::max(host_width >> h, 1u), lh = std::max(host_height >> h, 1u);
    const uint32_t blocks_x = (lw + bw - 1) / bw, blocks_y = (lh + bh - 1) / bh;
    const uint32_t pitch_blocks = bpb ? level_layout.row_pitch_bytes / bpb : 0;
    uint8_t* out = staging + level_offsets[h];
    for (uint32_t layer = 0; layer < layers; ++layer) {
      const uint64_t slice = source_offset + uint64_t(layer) * level_layout.array_slice_stride_bytes;
      for (uint32_t y = 0; y < blocks_y; ++y) {
        for (uint32_t x = 0; x < blocks_x; ++x, out += bpb) {
          uint64_t offset;
          if (fetch.tiled) {
            offset = slice + uint32_t(texture_util::GetTiledOffset2D(
                                 int32_t(x + packed_x), int32_t(y + packed_y), pitch_blocks,
                                 bpb_log2));
          } else {
            offset = slice + uint64_t(y + packed_y) * level_layout.row_pitch_bytes +
                     uint64_t(x + packed_x) * bpb;
          }
          if (offset + bpb <= source.size()) {
            std::memcpy(out, source.data() + offset, bpb);
          }
        }
      }
    }
    VkBufferImageCopy& r = regions[h];
    r.bufferOffset = staging_offset + level_offsets[h];
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, h, 0, layers};
    r.imageExtent = {lw, lh, 1};
  }

  VkCommandBuffer cmd = host.BeginTransferCommands();
  if (!cmd) {
    ++stats_.failures;
    return false;
  }
  const auto& dfn = device_->functions();
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = texture.initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = texture.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.levels, 0, texture.layers};
  dfn.vkCmdPipelineBarrier(cmd,
                           texture.initialized ? kShaderStages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  dfn.vkCmdCopyBufferToImage(cmd, staging_buffer, texture.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             std::min(host_levels, texture.levels), regions);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, kShaderStages, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);
  texture.initialized = true;
  texture.resolved_image = VK_NULL_HANDLE;
  ++stats_.loads;
  return true;
}

// --- Resolved surfaces ----------------------------------------------------------

void TextureCache::NoteResolve(uint32_t address, uint32_t length) {
  ResolveRecord& record = resolves_[address & kPhysicalMask];
  record.length = std::max(length, 1u);
  // Watching makes a later CPU write (the memory being reused for something
  // else) visible; the resolve itself doesn't touch guest memory.
  record.epoch = watcher_->WatchRange(address & kPhysicalMask, record.length);
  record.checked = record.epoch;
}

bool TextureCache::ResolvedValid(uint32_t address) {
  auto it = resolves_.find(address);
  if (it == resolves_.end()) {
    return false;
  }
  ResolveRecord& record = it->second;
  if (!watcher_->RangeUnchanged(address, record.length, record.epoch, record.checked)) {
    resolves_.erase(it);
    return false;
  }
  return resolved_->Find(address) != nullptr;
}

bool TextureCache::CopyFromResolved(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch,
                                    uint32_t address, Host& host) {
  ResolvedSurface* surface = resolved_->Find(address);
  if (!surface || !surface->write_count || !cmd_copy_image_) {
    return false;
  }
  // Only plain reinterpretations of the resolved texels (same block size).
  const FormatInfo* texture_info = FormatInfo::Get(fetch.format);
  const FormatInfo* surface_info =
      FormatInfo::Get(rex::graphics::ColorFormatToTextureFormat(surface->format));
  if (!texture_info || !surface_info || texture_info->block_width != 1 ||
      texture_info->bytes_per_block() != surface_info->bytes_per_block()) {
    return false;
  }
  if (texture.resolved_image == surface->image.image &&
      texture.resolved_writes == surface->write_count) {
    return true;
  }
  if (!texture.image) {
    uint32_t width_minus_1, height_minus_1;
    texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                   nullptr, nullptr, nullptr, nullptr, nullptr);
    texture.width = width_minus_1 + 1;
    texture.height = height_minus_1 + 1;
    texture.layers = texture.dimension == kCube ? 6 : 1;
    texture.levels = 1;
    if (!CreateImage(texture)) {
      texture.unsupported = true;
      ++stats_.failures;
      return false;
    }
  }
  VkCommandBuffer cmd = host.BeginTransferCommands();
  if (!cmd) {
    return false;
  }
  const auto& dfn = device_->functions();
  TransitionImage(device_, cmd, surface->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = texture.initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = texture.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.levels, 0, texture.layers};
  dfn.vkCmdPipelineBarrier(cmd,
                           texture.initialized ? kShaderStages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkImageCopy region = {};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {std::min(texture.width, surface->image.width),
                   std::min(texture.height, surface->image.height), 1};
  cmd_copy_image_(cmd, surface->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  dfn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, kShaderStages, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);
  texture.initialized = true;
  texture.resolved_image = surface->image.image;
  texture.resolved_writes = surface->write_count;
  return true;
}

// --- Views and samplers ---------------------------------------------------------

VkImageView TextureCache::GetView(Texture& texture, uint32_t swizzle) {
  if (auto it = texture.views.find(swizzle); it != texture.views.end()) {
    return it->second;
  }
  VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = texture.image;
  info.viewType = texture.dimension == kCube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
  info.format = texture.format;
  info.components.r = SwizzleComponent(swizzle & 7, texture.channels, texture.component_count);
  info.components.g =
      SwizzleComponent((swizzle >> 3) & 7, texture.channels, texture.component_count);
  info.components.b =
      SwizzleComponent((swizzle >> 6) & 7, texture.channels, texture.component_count);
  info.components.a =
      SwizzleComponent((swizzle >> 9) & 7, texture.channels, texture.component_count);
  info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.levels, 0, texture.layers};
  VkImageView view = VK_NULL_HANDLE;
  if (device_->functions().vkCreateImageView(device_->device(), &info, nullptr, &view) !=
      VK_SUCCESS) {
    ++stats_.failures;
    return VK_NULL_HANDLE;
  }
  texture.views.emplace(swizzle, view);
  return view;
}

VkSampler TextureCache::GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch) {
  using TF = xenos::TextureFilter;
  const uint64_t key = uint64_t(fetch.clamp_x) | (uint64_t(fetch.clamp_y) << 3) |
                       (uint64_t(fetch.clamp_z) << 6) | (uint64_t(fetch.mag_filter) << 9) |
                       (uint64_t(fetch.min_filter) << 11) | (uint64_t(fetch.mip_filter) << 13) |
                       (uint64_t(fetch.aniso_filter) << 15) |
                       (uint64_t(fetch.border_color) << 18) |
                       (uint64_t(uint32_t(fetch.lod_bias) & 0x3FF) << 20);
  if (auto it = samplers_.find(key); it != samplers_.end()) {
    return it->second;
  }
  const auto& props = device_->properties();
  VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = fetch.mag_filter == TF::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.minFilter = fetch.min_filter == TF::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  info.mipmapMode = fetch.mip_filter == TF::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                    : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = AddressMode(fetch.clamp_x, props.samplerMirrorClampToEdge);
  info.addressModeV = AddressMode(fetch.clamp_y, props.samplerMirrorClampToEdge);
  info.addressModeW = AddressMode(fetch.clamp_z, props.samplerMirrorClampToEdge);
  info.mipLodBias = float(fetch.lod_bias) / 32.0f;
  const uint32_t aniso = uint32_t(fetch.aniso_filter);
  if (props.samplerAnisotropy && aniso >= uint32_t(xenos::AnisoFilter::kMax_2_1) &&
      aniso <= uint32_t(xenos::AnisoFilter::kMax_16_1)) {
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = std::min(float(1u << (aniso - 1)), props.maxSamplerAnisotropy);
  }
  info.minLod = 0.0f;
  info.maxLod = fetch.mip_filter == TF::kBaseMap ? 0.25f : VK_LOD_CLAMP_NONE;
  info.borderColor = fetch.border_color == xenos::BorderColor::k_ABGR_White
                         ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                         : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler sampler = VK_NULL_HANDLE;
  if (device_->functions().vkCreateSampler(device_->device(), &info, nullptr, &sampler) !=
      VK_SUCCESS) {
    ++stats_.failures;
    return VK_NULL_HANDLE;
  }
  samplers_.emplace(key, sampler);
  return sampler;
}

}  // namespace mantra
