#include "render_targets.h"
#include "mantra_log.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <rex/math.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

namespace mantra {

namespace vk = rex::ui::vulkan;

namespace {

constexpr VkFormat kColor32Formats[] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16_SFLOAT,  VK_FORMAT_R32_SFLOAT,
};
constexpr VkFormat kColor64Formats[] = {
    VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R16G16B16A16_SNORM,
    VK_FORMAT_R32G32_SFLOAT,
};

VkSampleCountFlagBits ToSampleCount(xenos::MsaaSamples msaa) {
  switch (msaa) {
    case xenos::MsaaSamples::k2X: return VK_SAMPLE_COUNT_2_BIT;
    case xenos::MsaaSamples::k4X: return VK_SAMPLE_COUNT_4_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
  }
}

// Unpacked 16-bit fixed point (-32...32 on Xenos) clamped into SNORM's -1...1.
float Fixed16ToSnorm(uint16_t v) { return std::max(float(int16_t(v)) * (1.0f / 32767.0f), -1.0f); }

}  // namespace

// --- RenderTargetPool --------------------------------------------------------

bool RenderTargetPool::Initialize(const vk::VulkanDevice* device) {
  device_ = device;
  const auto& ifn = device->vulkan_instance()->functions();
  for (VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
    VkFormatProperties props = {};
    ifn.vkGetPhysicalDeviceFormatProperties(device->physical_device(), candidate, &props);
    if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
      depth_format_ = candidate;
      break;
    }
  }
  if (depth_format_ == VK_FORMAT_UNDEFINED) {
    MANTRA_ERROR("no supported depth/stencil format");
    return false;
  }
  return true;
}

void RenderTargetPool::Shutdown() {
  if (!device_) {
    return;
  }
  const auto& dfn = device_->functions();
  for (auto& [hash, rt] : targets_) {
    for (auto& [format, view] : rt->views) {
      dfn.vkDestroyImageView(device_->device(), view, nullptr);
    }
    DestroyTrackedImage(device_, rt->image);
  }
  targets_.clear();
  device_ = nullptr;
}

VkFormat RenderTargetPool::HostColorFormat(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA: return VK_FORMAT_R8G8B8A8_UNORM;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    // 7e3 has no host equivalent; fp16 keeps its range and precision.
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_16_16: return VK_FORMAT_R16G16_SNORM;
    case F::k_16_16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case F::k_32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case F::k_16_16_16_16: return VK_FORMAT_R16G16B16A16_SNORM;
    case F::k_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_32_32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
  }
  return VK_FORMAT_R8G8B8A8_UNORM;
}

RtClass RenderTargetPool::HostColorClass(xenos::ColorRenderTargetFormat format) {
  switch (HostColorFormat(format)) {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R32G32_SFLOAT: return RtClass::kColor64;
    default: return RtClass::kColor32;
  }
}

RenderTarget* RenderTargetPool::GetColor(VkCommandBuffer cmd, uint32_t base_tiles,
                                         uint32_t pitch_px, xenos::MsaaSamples msaa,
                                         xenos::ColorRenderTargetFormat format, uint64_t frame) {
  RtKey key{base_tiles & (xenos::kEdramTileCount - 1), pitch_px, msaa, HostColorClass(format)};
  auto it = targets_.find(key.Hash());
  RenderTarget* rt = it != targets_.end() ? it->second.get() : Create(cmd, key);
  if (rt) {
    rt->last_used_frame = frame;
  }
  return rt;
}

RenderTarget* RenderTargetPool::GetDepth(VkCommandBuffer cmd, uint32_t base_tiles,
                                         uint32_t pitch_px, xenos::MsaaSamples msaa,
                                         uint64_t frame) {
  RtKey key{base_tiles & (xenos::kEdramTileCount - 1), pitch_px, msaa, RtClass::kDepth};
  auto it = targets_.find(key.Hash());
  RenderTarget* rt = it != targets_.end() ? it->second.get() : Create(cmd, key);
  if (rt) {
    rt->last_used_frame = frame;
  }
  return rt;
}

RenderTarget* RenderTargetPool::Create(VkCommandBuffer cmd, const RtKey& key) {
  if (!key.pitch_px) {
    MANTRA_WARN_ONCE("render target with zero surface pitch");
    return nullptr;
  }
  const auto& props = device_->properties();
  // The image covers every EDRAM row reachable from the base tile, assuming
  // 32bpp (the larger height); 64bpp surfaces use the upper part.
  const uint32_t pitch_tiles = xenos::GetSurfacePitchTiles(key.pitch_px, key.msaa, false);
  const uint32_t tile_rows = (xenos::kEdramTileCount - key.base_tiles) / std::max(pitch_tiles, 1u);
  const uint32_t rows_log2 = key.msaa >= xenos::MsaaSamples::k2X ? 1 : 0;
  const uint32_t max_dim = std::min<uint32_t>(props.maxImageDimension2D, 2560);
  const uint32_t width = std::min(key.pitch_px, max_dim);
  const uint32_t height =
      std::clamp<uint32_t>((tile_rows * xenos::kEdramTileHeightSamples) >> rows_log2, 8, max_dim);
  const VkSampleCountFlagBits samples = ToSampleCount(key.msaa);

  auto rt = std::make_unique<RenderTarget>();
  rt->key = key;
  bool created;
  if (key.cls == RtClass::kDepth) {
    if (!(props.framebufferDepthSampleCounts & samples)) {
      MANTRA_WARN_ONCE("{}x MSAA depth is not supported by the host", uint32_t(samples));
      return nullptr;
    }
    created = CreateTrackedImage(
        device_, depth_format_, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, width,
        height, samples,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        nullptr, 0, rt->image);
  } else {
    if (!(props.framebufferColorSampleCounts & samples)) {
      MANTRA_WARN_ONCE("{}x MSAA color is not supported by the host", uint32_t(samples));
      return nullptr;
    }
    const bool is_64 = key.cls == RtClass::kColor64;
    const VkFormat* formats = is_64 ? kColor64Formats : kColor32Formats;
    const uint32_t format_count = is_64 ? uint32_t(std::size(kColor64Formats))
                                        : uint32_t(std::size(kColor32Formats));
    created = CreateTrackedImage(device_, formats[0], VK_IMAGE_ASPECT_COLOR_BIT, width, height,
                                 samples,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 formats, format_count, rt->image);
  }
  if (!created) {
    MANTRA_ERROR("failed to create a {}x{} {}x render target (base {} pitch {})", width, height,
                 uint32_t(samples), key.base_tiles, key.pitch_px);
    return nullptr;
  }
  MANTRA_DEBUG("render target: base {} pitch {} msaa {} {} -> {}x{}", key.base_tiles,
               key.pitch_px, 1u << uint32_t(key.msaa),
               key.cls == RtClass::kDepth ? "depth" : key.cls == RtClass::kColor64 ? "color64"
                                                                                    : "color32",
               width, height);

  // Start from zero rather than garbage.
  VkClearValue zero = {};
  VkRect2D whole = {{0, 0}, {width, height}};
  RenderTarget* result = rt.get();
  targets_.emplace(key.Hash(), std::move(rt));
  ClearAttachment(cmd, *result, GetView(*result, result->image.format), whole, zero);
  return result;
}

VkImageView RenderTargetPool::GetView(RenderTarget& rt, VkFormat format) {
  if (auto it = rt.views.find(format); it != rt.views.end()) {
    return it->second;
  }
  VkImageView view = CreateImageView(device_, rt.image.image, format, rt.image.aspect);
  if (!view) {
    MANTRA_ERROR("failed to create a render target view of format {}", int32_t(format));
  }
  rt.views.emplace(format, view);
  return view;
}

void RenderTargetPool::ClearAttachment(VkCommandBuffer cmd, RenderTarget& rt, VkImageView view,
                                       const VkRect2D& rect, const VkClearValue& value,
                                       VkImageAspectFlags aspects) {
  if (!view) {
    return;
  }
  // Clamp to the image.
  VkRect2D clamped = rect;
  clamped.offset.x = std::clamp<int32_t>(rect.offset.x, 0, int32_t(rt.image.width));
  clamped.offset.y = std::clamp<int32_t>(rect.offset.y, 0, int32_t(rt.image.height));
  clamped.extent.width =
      std::min<uint32_t>(rect.extent.width, rt.image.width - uint32_t(clamped.offset.x));
  clamped.extent.height =
      std::min<uint32_t>(rect.extent.height, rt.image.height - uint32_t(clamped.offset.y));
  if (!clamped.extent.width || !clamped.extent.height) {
    return;
  }
  const auto& dfn = device_->functions();
  const bool is_depth = rt.key.cls == RtClass::kDepth;
  if (is_depth) {
    TransitionImage(device_, cmd, rt.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    kDepthAttachmentStage, kDepthAttachmentAccess);
  } else {
    TransitionImage(device_, cmd, rt.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    kColorAttachmentStage, kColorAttachmentAccess);
  }

  VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView = view;
  attachment.imageLayout = rt.image.layout;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea = clamped;
  rendering.layerCount = 1;
  if (is_depth) {
    rendering.pDepthAttachment = &attachment;
    rendering.pStencilAttachment = &attachment;
  } else {
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
  }
  dfn.vkCmdBeginRendering(cmd, &rendering);
  VkClearAttachment clear = {};
  clear.aspectMask = aspects ? aspects
                     : is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                : VK_IMAGE_ASPECT_COLOR_BIT;
  clear.colorAttachment = 0;
  clear.clearValue = value;
  VkClearRect clear_rect = {};
  clear_rect.rect = clamped;
  clear_rect.layerCount = 1;
  dfn.vkCmdClearAttachments(cmd, 1, &clear, 1, &clear_rect);
  dfn.vkCmdEndRendering(cmd);
}

void RenderTargetPool::ClearColor(VkCommandBuffer cmd, RenderTarget& rt,
                                  xenos::ColorRenderTargetFormat format, const VkRect2D& rect,
                                  uint64_t clear_value) {
  using F = xenos::ColorRenderTargetFormat;
  VkClearValue value = {};
  float* c = value.color.float32;
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA:
      for (uint32_t j = 0; j < 4; ++j) c[j] = float((clear_value >> (j * 8)) & 0xFF) / 255.0f;
      break;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      for (uint32_t j = 0; j < 3; ++j) c[j] = float((clear_value >> (j * 10)) & 0x3FF) / 1023.0f;
      c[3] = float((clear_value >> 30) & 0x3) / 3.0f;
      break;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      for (uint32_t j = 0; j < 3; ++j) {
        c[j] = xenos::Float7e3To32(uint32_t(clear_value >> (j * 10)) & 0x3FF);
      }
      c[3] = float((clear_value >> 30) & 0x3) / 3.0f;
      break;
    case F::k_16_16:
    case F::k_16_16_16_16:
      for (uint32_t j = 0; j < (format == F::k_16_16 ? 2u : 4u); ++j) {
        c[j] = Fixed16ToSnorm(uint16_t(clear_value >> (j * 16)));
      }
      break;
    case F::k_16_16_FLOAT:
    case F::k_16_16_16_16_FLOAT:
      for (uint32_t j = 0; j < (format == F::k_16_16_FLOAT ? 2u : 4u); ++j) {
        c[j] = rex::xenos_half_to_float(uint16_t(clear_value >> (j * 16)));
      }
      break;
    case F::k_32_FLOAT:
    case F::k_32_32_FLOAT: {
      uint32_t bits[2] = {uint32_t(clear_value), uint32_t(clear_value >> 32)};
      std::memcpy(c, bits, sizeof(bits));
    } break;
  }
  ClearAttachment(cmd, rt, GetView(rt, HostColorFormat(format)), rect, value);
}

void RenderTargetPool::ClearColorFloat(VkCommandBuffer cmd, RenderTarget& rt,
                                       xenos::ColorRenderTargetFormat format, const VkRect2D& rect,
                                       const float color[4]) {
  VkClearValue value = {};
  std::memcpy(value.color.float32, color, sizeof(value.color.float32));
  ClearAttachment(cmd, rt, GetView(rt, HostColorFormat(format)), rect, value);
}

void RenderTargetPool::ClearDepthStencil(VkCommandBuffer cmd, RenderTarget& rt,
                                         const VkRect2D& rect, bool clear_depth, float depth,
                                         bool clear_stencil, uint32_t stencil) {
  VkClearValue value = {};
  value.depthStencil.depth = depth;
  value.depthStencil.stencil = stencil & 0xFF;
  const VkImageAspectFlags aspects = (clear_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                     (clear_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
  if (aspects) {
    ClearAttachment(cmd, rt, GetView(rt, depth_format_), rect, value, aspects);
  }
}

void RenderTargetPool::ClearDepth(VkCommandBuffer cmd, RenderTarget& rt,
                                  xenos::DepthRenderTargetFormat format, const VkRect2D& rect,
                                  uint32_t clear_value) {
  VkClearValue value = {};
  const uint32_t depth24 = (clear_value >> 8) & 0xFFFFFF;
  value.depthStencil.depth = format == xenos::DepthRenderTargetFormat::kD24FS8
                                 ? xenos::Float20e4To32(depth24) * 0.5f
                                 : xenos::UNorm24To32(depth24);
  value.depthStencil.stencil = clear_value & 0xFF;
  ClearAttachment(cmd, rt, GetView(rt, depth_format_), rect, value);
}

// --- ResolvedSurfaces --------------------------------------------------------

VkFormat ResolvedSurfaces::HostFormat(xenos::ColorFormat format) {
  using F = xenos::ColorFormat;
  switch (format) {
    case F::k_8: return VK_FORMAT_R8_UNORM;
    case F::k_8_8: return VK_FORMAT_R8G8_UNORM;
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16: return VK_FORMAT_R8G8B8A8_UNORM;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case F::k_16: return VK_FORMAT_R16_UNORM;
    case F::k_16_16: return VK_FORMAT_R16G16_UNORM;
    case F::k_16_16_16_16: return VK_FORMAT_R16G16B16A16_UNORM;
    case F::k_16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case F::k_16_16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case F::k_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case F::k_32_32_32_32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
  }
}

void ResolvedSurfaces::Destroy(ResolvedSurface& surface) {
  if (surface.view) {
    device_->functions().vkDestroyImageView(device_->device(), surface.view, nullptr);
    surface.view = VK_NULL_HANDLE;
  }
  DestroyTrackedImage(device_, surface.image);
}

void ResolvedSurfaces::Shutdown() {
  if (!device_) {
    return;
  }
  for (auto& [address, surface] : surfaces_) Destroy(*surface);
  for (auto& [frame, surface] : retired_) Destroy(*surface);
  surfaces_.clear();
  retired_.clear();
  device_ = nullptr;
}

void ResolvedSurfaces::Collect(uint64_t completed_frame) {
  std::erase_if(retired_, [&](auto& entry) {
    if (entry.first > completed_frame) {
      return false;
    }
    Destroy(*entry.second);
    return true;
  });
}

ResolvedSurface* ResolvedSurfaces::Find(uint32_t address) {
  auto it = surfaces_.find(address);
  return it != surfaces_.end() ? it->second.get() : nullptr;
}

ResolvedSurface* ResolvedSurfaces::Get(uint32_t address, uint32_t pitch, uint32_t height,
                                       xenos::ColorFormat format, uint64_t frame) {
  const VkFormat host_format = HostFormat(format);
  if (host_format == VK_FORMAT_UNDEFINED) {
    return nullptr;
  }
  const uint32_t max_dim = device_->properties().maxImageDimension2D;
  pitch = std::clamp<uint32_t>(pitch, 1, max_dim);
  height = std::clamp<uint32_t>(height, 1, max_dim);
  if (auto it = surfaces_.find(address); it != surfaces_.end()) {
    ResolvedSurface& existing = *it->second;
    if (existing.format == format && existing.image.width >= pitch &&
        existing.image.height >= height) {
      existing.pitch = pitch;
      existing.height = height;
      return &existing;
    }
    // Different format or too small: replace; the old image may still be in
    // use by in-flight submissions.
    pitch = std::max(pitch, existing.image.width);
    height = std::max(height, existing.image.height);
    retired_.emplace_back(frame, std::move(it->second));
    surfaces_.erase(it);
  }

  auto surface = std::make_unique<ResolvedSurface>();
  surface->address = address;
  surface->pitch = pitch;
  surface->height = height;
  surface->format = format;
  if (!CreateTrackedImage(device_, host_format, VK_IMAGE_ASPECT_COLOR_BIT, pitch, height,
                          VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          nullptr, 0, surface->image)) {
    MANTRA_ERROR("failed to create a {}x{} resolved surface (format {})", pitch, height,
                 uint32_t(format));
    return nullptr;
  }
  surface->view = CreateImageView(device_, surface->image.image, host_format,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
  if (!surface->view) {
    Destroy(*surface);
    return nullptr;
  }
  MANTRA_DEBUG("resolved surface 0x{:08X}: {}x{} format {}", address, pitch, height,
               uint32_t(format));
  ResolvedSurface* result = surface.get();
  surfaces_.emplace(address, std::move(surface));
  return result;
}

}  // namespace mantra
