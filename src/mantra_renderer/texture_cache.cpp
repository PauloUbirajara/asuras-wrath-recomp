#include "texture_cache.h"
#include "buffer_cache.h"
#include "mantra_log.h"
#include "render_targets.h"

#include <algorithm>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <string>

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

// How guest blocks become host data. kCopy and kDXT3A keep the guest block
// grid; the others write one host block per texel.
enum Decode : uint8_t {
  kCopy,
  kCTX1,       // 4x4 8-byte blocks -> R8G8 texels
  kDXT3A,      // 4x4 alpha-only blocks -> BC2 with black color
  kR11G11B10,  // k_10_11_11 -> RGBA16
  kR10G11B11,  // k_11_11_10 -> RGBA16
  kDepthUnorm, // k_24_8 -> R32 float
  kDepthFloat, // k_24_8_FLOAT (20e4) -> R32 float
  kYUV,        // 4:2:2 (2x1 blocks, bytes Y0 Cb Y1 Cr) -> RGBA8 (Cr, Y, Cb, 1)
  kYUVSwapped, // 4:2:2, bytes Cb Y0 Cr Y1
};

struct HostFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  Decode decode = kCopy;
  uint32_t host_bpb = 0;  // 0: same as the guest block
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
    case F::k_10_11_11:
    case F::k_11_11_10:
      h.format = pick(all_signed || (x_signed && fetch.sign_y == S::kSigned &&
                                     fetch.sign_z == S::kSigned),
                      VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM);
      h.decode = rex::graphics::GetBaseFormat(fetch.format) == F::k_10_11_11 ? kR11G11B10
                                                                              : kR10G11B11;
      h.host_bpb = 8;
      h.component_count = 3;
      break;
    case F::k_24_8:
    case F::k_24_8_FLOAT:
      h.format = VK_FORMAT_R32_SFLOAT;
      h.decode = fetch.format == F::k_24_8 ? kDepthUnorm : kDepthFloat;
      h.host_bpb = 4;
      h.component_count = 1;
      break;
    case F::k_CTX1:
      h.format = VK_FORMAT_R8G8_UNORM;
      h.decode = kCTX1;
      h.host_bpb = 2;
      h.component_count = 2;
      break;
    case F::k_DXT3A:
      h.format = VK_FORMAT_BC2_UNORM_BLOCK;
      h.decode = kDXT3A;
      h.host_bpb = 16;
      h.channels[0] = VK_COMPONENT_SWIZZLE_A;
      h.component_count = 1;
      break;
    case F::k_Cr_Y1_Cb_Y0_REP:
    case F::k_Y1_Cr_Y0_Cb_REP:
      h.format = VK_FORMAT_R8G8B8A8_UNORM;
      h.decode = fetch.format == F::k_Cr_Y1_Cb_Y0_REP ? kYUV : kYUVSwapped;
      h.host_bpb = 4;
      h.component_count = 3;
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

// Fixed-point component of `bits` bits -> 16-bit UNORM/SNORM.
uint16_t ExpandComponent(uint32_t value, uint32_t bits, bool is_signed) {
  if (!is_signed) {
    const uint32_t max = (1u << bits) - 1;
    return uint16_t((value * 65535u + max / 2) / max);
  }
  int32_t v = int32_t(value << (32 - bits)) >> (32 - bits);
  const int32_t max = (1 << (bits - 1)) - 1;
  v = std::max(v, -max);
  return uint16_t(int16_t((v * 32767 + (v < 0 ? -max / 2 : max / 2)) / max));
}

// Decodes one guest block (already endian-swapped) into host blocks laid out
// row by row, `blocks_x` per row.
void DecodeBlock(Decode decode, bool is_signed, const uint8_t* src, uint8_t* dst,
                 uint32_t blocks_x) {
  uint32_t word;
  std::memcpy(&word, src, 4);
  switch (decode) {
    case kCTX1:
      rex::graphics::texture_conversion::ConvertTexelCTX1ToR8G8(xenos::Endian::kNone, dst, src,
                                                               blocks_x * 2);
      break;
    case kDXT3A:
      rex::graphics::texture_conversion::ConvertTexelDXT3AToDXT3(xenos::Endian::kNone, dst, src,
                                                                16);
      break;
    case kR11G11B10:
    case kR10G11B11: {
      const uint32_t bx = decode == kR11G11B10 ? 11 : 10, bz = decode == kR11G11B10 ? 10 : 11;
      const uint16_t out[4] = {
          ExpandComponent(word & ((1u << bx) - 1), bx, is_signed),
          ExpandComponent((word >> bx) & 0x7FF, 11, is_signed),
          ExpandComponent(word >> (bx + 11), bz, is_signed),
          uint16_t(is_signed ? 0x7FFF : 0xFFFF)};
      std::memcpy(dst, out, 8);
      break;
    }
    case kDepthUnorm: {
      const float depth = float(word >> 8) * (1.0f / 16777215.0f);
      std::memcpy(dst, &depth, 4);
      break;
    }
    case kDepthFloat: {
      const float depth = xenos::Float20e4To32(word >> 8);
      std::memcpy(dst, &depth, 4);
      break;
    }
    case kYUV:
    case kYUVSwapped: {
      // kYUV bytes: Y0 Cb Y1 Cr; kYUVSwapped: Cb Y0 Cr Y1.
      const bool swapped = decode == kYUVSwapped;
      const uint8_t y0 = src[swapped ? 1 : 0], cb = src[swapped ? 0 : 1];
      const uint8_t y1 = src[swapped ? 3 : 2], cr = src[swapped ? 2 : 3];
      const uint8_t out[8] = {cr, y0, cb, 255, cr, y1, cb, 255};
      std::memcpy(dst, out, 8);
      break;
    }
    case kCopy:
      break;
  }
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
  const bool stacked = fetch.dimension == xenos::DataDimension::k2DOrStacked && fetch.stacked &&
                       fetch.size_2d.stack_depth > 0;
  binding.dimension = fetch.dimension == xenos::DataDimension::kCube ? kCube
                      : fetch.dimension == xenos::DataDimension::k3D || stacked ? k3D
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
    texture.decode = host_format.decode;
    texture.host_bpb = uint8_t(host_format.host_bpb);
    texture.dimension = binding.dimension;
    texture.stacked = stacked;
    {
      uint32_t w1, h1, d1;
      texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1, nullptr, nullptr,
                                                     nullptr, nullptr);
      MANTRA_DEBUG("texture {:08X}: format {} dimension {} {}x{}x{} tiled {} sign {} swizzle {:03o}",
                   fetch.base_address << 12, uint32_t(fetch.format), uint32_t(fetch.dimension),
                   w1 + 1, h1 + 1, d1 + 1, uint32_t(fetch.tiled), uint32_t(fetch.sign_x),
                   uint32_t(fetch.swizzle));
    }
    if (texture.format == VK_FORMAT_UNDEFINED) {
      texture.unsupported = true;
      const uint32_t format = uint32_t(fetch.format) & 63;
      if (!(unsupported_logged_ & (uint64_t(1) << format))) {
        unsupported_logged_ |= uint64_t(1) << format;
        MANTRA_INFO("texture format {} not supported yet (placeholder)", format);
      }
    }
  }
  if (texture.unsupported) {
    ++stats_.unsupported;
    return binding;
  }

  const uint32_t base_address = (fetch.base_address << 12) & kPhysicalMask;
  bool ready = texture.initialized;
  bool from_resolve = false;
  if (base_address && ResolvedValid(base_address)) {
    from_resolve = CopyFromResolved(texture, fetch, base_address, host);
    if (from_resolve) {
      ready = true;
      ++stats_.resolved_binds;
    } else {
      ++stats_.resolve_mismatches;
    }
  }
  if (!from_resolve && (texture.resolved_image != VK_NULL_HANDLE || !texture.initialized ||
             !watcher_->RangeUnchanged(texture.base_start, texture.base_length, texture.epoch,
                                       texture.base_checked) ||
             !watcher_->RangeUnchanged(texture.mip_start, texture.mip_length, texture.epoch,
                                       texture.mip_checked))) {
    if (texture.initialized && texture.resolved_image == VK_NULL_HANDLE) {
      ++stats_.reloads;
    }
    ready = LoadFromMemory(texture, fetch, host);
  }
  if (!ready) {
    return binding;
  }
  binding.view = GetView(texture, fetch.swizzle);
  if (from_resolve) {
    using F = xenos::TextureFormat;
    const F base = rex::graphics::GetBaseFormat(fetch.format);
    // Debug: MANTRA_NO_FIXED16_BIAS=1 reads them unscaled.
    static const bool no_bias = std::getenv("MANTRA_NO_FIXED16_BIAS") != nullptr;
    if (!no_bias && (base == F::k_16 || base == F::k_16_16 || base == F::k_16_16_16_16)) {
      // SNORM views read value / 32; UNORM views of the same bits read
      // value * 1024 / 65535 (non-negative values only).
      const bool snorm = texture.format == VK_FORMAT_R16_SNORM ||
                         texture.format == VK_FORMAT_R16G16_SNORM ||
                         texture.format == VK_FORMAT_R16G16B16A16_SNORM;
      binding.exp_bias = snorm ? 5 : 6;
      static std::unordered_set<uint64_t> logged;
      const uint64_t log_key = (uint64_t(base_address) << 8) | (uint32_t(fetch.sign_x) << 6) |
                               (uint32_t(fetch.sign_y) << 4) | (uint32_t(fetch.sign_z) << 2) |
                               uint32_t(fetch.sign_w);
      if (logged.insert(log_key).second) {
        MANTRA_INFO("fixed16 resolve read {:08X} fmt {} sign {}{}{}{} -> {}", base_address,
                    uint32_t(fetch.format), uint32_t(fetch.sign_x), uint32_t(fetch.sign_y),
                    uint32_t(fetch.sign_z), uint32_t(fetch.sign_w), snorm ? "snorm" : "unorm");
      }
    }
  }
  return binding;
}

// --- Loading ----------------------------------------------------------------

bool TextureCache::CreateImage(Texture& texture) {
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = texture.dimension == kCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  info.imageType = texture.dimension == k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  info.format = texture.format;
  info.extent = {texture.width, texture.height, texture.depth};
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
  const uint32_t layers = texture.dimension == kCube ? 6 : 1;
  // Guest 3D tiling; stacked slices are 2D-tiled, array_slice_stride apart.
  const bool is_3d = texture.dimension == k3D && !texture.stacked;
  const uint32_t depth = texture.dimension == k3D ? array_size : 1;
  if (texture.stacked) {
    mip_max = mip_min;  // a host 3D mip chain would halve the slice count
  }
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
    texture.depth = std::max(depth >> mip_min, 1u);
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

  // Staging: every level, all layers / slices, tightly packed, 16-byte
  // aligned. Host blocks are the guest blocks, or texels for decoded formats.
  const uint32_t bw = format_info->block_width, bh = format_info->block_height;
  const uint32_t bpb = format_info->bytes_per_block();
  const uint32_t bpb_log2 = rex::log2_floor(bpb);
  const Decode decode = Decode(texture.decode);
  const bool keeps_blocks = decode == kCopy || decode == kDXT3A;
  const uint32_t hbw = keeps_blocks ? bw : 1, hbh = keeps_blocks ? bh : 1;
  const uint32_t hbpb = texture.host_bpb ? texture.host_bpb : bpb;
  const uint32_t nx = bw / hbw, ny = bh / hbh;  // host blocks per guest block
  const bool decode_signed = fetch.sign_x == xenos::TextureSign::kSigned;
  uint32_t staging_size = 0;
  uint32_t level_offsets[xenos::kTextureMaxMips + 1];
  for (uint32_t h = 0; h < host_levels; ++h) {
    level_offsets[h] = staging_size;
    const uint32_t lw = std::max(host_width >> h, 1u), lh = std::max(host_height >> h, 1u);
    const uint32_t ld = std::max(texture.depth >> h, 1u);
    staging_size += ((lw + hbw - 1) / hbw) * ((lh + hbh - 1) / hbh) * hbpb * layers * ld;
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
  uint8_t decoded[64];
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
      texture_util::GetPackedMipOffset(width, height, depth, fetch.format, level, packed_x,
                                       packed_y, packed_z);
    }
    const uint32_t lw = std::max(host_width >> h, 1u), lh = std::max(host_height >> h, 1u);
    const uint32_t ld = std::max(texture.depth >> h, 1u);
    const uint32_t blocks_x = (lw + bw - 1) / bw, blocks_y = (lh + bh - 1) / bh;
    const uint32_t host_blocks_x = (lw + hbw - 1) / hbw, host_blocks_y = (lh + hbh - 1) / hbh;
    const uint32_t pitch_blocks = bpb ? level_layout.row_pitch_bytes / bpb : 0;
    const uint32_t slice_rows = level_layout.z_slice_stride_block_rows;
    uint8_t* const level_out = staging + level_offsets[h];
    for (uint32_t layer = 0; layer < layers; ++layer) {
      for (uint32_t z = 0; z < ld; ++z) {
        const uint64_t slice =
            source_offset + uint64_t(texture.stacked ? z : layer) * level_layout.array_slice_stride_bytes;
        uint8_t* const plane_out =
            level_out + uint64_t(layer * ld + z) * host_blocks_y * host_blocks_x * hbpb;
        for (uint32_t y = 0; y < blocks_y; ++y) {
          for (uint32_t x = 0; x < blocks_x; ++x) {
            const int32_t sx = int32_t(x + packed_x), sy = int32_t(y + packed_y);
            const int32_t sz = texture.stacked ? 0 : int32_t(z + packed_z);
            uint64_t offset;
            if (fetch.tiled) {
              offset = slice + uint32_t(is_3d ? texture_util::GetTiledOffset3D(
                                                    sx, sy, sz, pitch_blocks, slice_rows, bpb_log2)
                                              : texture_util::GetTiledOffset2D(sx, sy, pitch_blocks,
                                                                               bpb_log2));
            } else {
              offset = slice + (uint64_t(sz) * slice_rows + uint64_t(sy)) *
                                   level_layout.row_pitch_bytes +
                       uint64_t(sx) * bpb;
            }
            if (offset + bpb > source.size()) {
              continue;
            }
            const uint8_t* block = source.data() + offset;
            if (decode == kCopy) {
              std::memcpy(plane_out + (uint64_t(y) * host_blocks_x + x) * hbpb, block, bpb);
              continue;
            }
            DecodeBlock(decode, decode_signed, block, decoded, nx);
            // Clip the decoded texels to the level (partial edge blocks).
            for (uint32_t dy = 0; dy < ny && y * ny + dy < host_blocks_y; ++dy) {
              const uint32_t count = std::min(nx, host_blocks_x - x * nx);
              std::memcpy(plane_out + (uint64_t(y * ny + dy) * host_blocks_x + x * nx) * hbpb,
                          decoded + dy * nx * hbpb, count * hbpb);
            }
          }
        }
      }
    }
    VkBufferImageCopy& r = regions[h];
    r.bufferOffset = staging_offset + level_offsets[h];
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, h, 0, layers};
    r.imageExtent = {lw, lh, ld};
  }

  // Debug: MANTRA_TEXTURE_DUMP_DIR gets level 0 of 8-bit textures (PGM)
  // every 60th load of each address.
  if (const char* dump_dir = std::getenv("MANTRA_TEXTURE_DUMP_DIR");
      dump_dir && texture.format == VK_FORMAT_R8_UNORM && host_levels &&
      (texture.dump_counter++ % 60) == 0) {
    const std::string path = fmt::format("{}/tex_{:08X}_{:05}.pgm", dump_dir, base_address,
                                         texture.dump_counter - 1);
    if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
      std::fprintf(file, "P5\n%u %u\n255\n", host_width, host_height);
      std::fwrite(staging + level_offsets[0], 1, size_t(host_width) * host_height, file);
      std::fclose(file);
    }
  }
  // Debug: and the raw level 0 blocks of BC1 textures, once per address.
  if (const char* dump_dir = std::getenv("MANTRA_TEXTURE_DUMP_DIR");
      dump_dir && host_levels &&
      (texture.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
       texture.format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK) &&
      texture.dump_counter++ == 0) {
    const std::string path =
        fmt::format("{}/tex_{:08X}_{}x{}_{}.bc1", dump_dir, base_address, host_width, host_height,
                    texture.format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ? "srgb" : "unorm");
    if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
      std::fwrite(staging + level_offsets[0], 1,
                  size_t((host_width + 3) / 4) * ((host_height + 3) / 4) * 8, file);
      std::fclose(file);
    }
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
  if (!surface || !surface->write_count || !cmd_copy_image_ ||
      (texture.dimension == k3D && !texture.stacked)) {
    return false;
  }
  if (surface->depth) {
    // Depth resolves hold decoded depth (R32F), like the 24:8 decoders.
    if ((texture.decode != kDepthUnorm && texture.decode != kDepthFloat) || texture.stacked) {
      return false;
    }
  } else {
    if (texture.decode != kCopy) {
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
  }
  if (texture.resolved_image == surface->image.image &&
      texture.resolved_writes == surface->write_count) {
    return true;
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, nullptr, nullptr, nullptr,
                                                 nullptr);
  // Stacked slices sit array_slice_stride apart in the resolved surface's rows.
  uint32_t slice_rows = 0;
  if (texture.stacked) {
    const auto layout = texture_util::GetGuestTextureLayout(
        fetch.dimension, fetch.pitch, width_minus_1 + 1, height_minus_1 + 1, depth_minus_1 + 1,
        fetch.tiled, fetch.format, fetch.packed_mips, true, 0);
    if (!layout.base.row_pitch_bytes) {
      return false;
    }
    slice_rows = layout.base.array_slice_stride_bytes / layout.base.row_pitch_bytes;
    if (!slice_rows || slice_rows * depth_minus_1 >= surface->image.height) {
      MANTRA_WARN_ONCE("stacked texture {:08X}: {} slices of {} rows don't fit the {}-row resolve",
                       address, depth_minus_1 + 1, slice_rows, surface->image.height);
      return false;
    }
  }
  if (!texture.image) {
    texture.width = width_minus_1 + 1;
    texture.height = height_minus_1 + 1;
    texture.depth = texture.stacked ? depth_minus_1 + 1 : 1;
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
  VkImageCopy regions[64] = {};
  const uint32_t region_count = texture.stacked ? std::min(texture.depth, 64u) : 1;
  for (uint32_t z = 0; z < region_count; ++z) {
    VkImageCopy& region = regions[z];
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffset = {0, int32_t(z * slice_rows), 0};
    region.dstOffset = {0, 0, int32_t(z)};
    region.extent = {std::min(texture.width, surface->image.width),
                     std::min(texture.height, surface->image.height - z * slice_rows), 1};
  }
  cmd_copy_image_(cmd, surface->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region_count, regions);
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
  info.viewType = texture.dimension == kCube ? VK_IMAGE_VIEW_TYPE_CUBE
                  : texture.dimension == k3D ? VK_IMAGE_VIEW_TYPE_3D
                                             : VK_IMAGE_VIEW_TYPE_2D;
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
