#pragma once
// Host replacements for Xenos EDRAM.
//
// RenderTargetPool: one Vulkan image per EDRAM render-target configuration,
// keyed by (base tile, surface pitch, MSAA, host size class) but not by guest
// format: games reuse one EDRAM region with several formats, so 32bpp formats
// share one mutable-format image (likewise 64bpp). Contents are not carried
// over between different keys that alias the same EDRAM tiles.
//
// ResolvedSurfaces: the destinations of resolves (EDRAM -> texture copies),
// kept on the GPU and keyed by guest address. The frontbuffer present and
// (later) the texture cache read them instead of guest memory.
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/xenos.h>

#include "vk_image.h"

namespace mantra {

namespace xenos = rex::graphics::xenos;

enum class RtClass : uint8_t { kColor32, kColor64, kDepth };

struct RtKey {
  uint32_t base_tiles = 0;
  uint32_t pitch_px = 0;
  xenos::MsaaSamples msaa = xenos::MsaaSamples::k1X;
  RtClass cls = RtClass::kColor32;
  bool operator==(const RtKey&) const = default;
  uint64_t Hash() const {
    return uint64_t(base_tiles) | (uint64_t(pitch_px) << 12) | (uint64_t(msaa) << 28) |
           (uint64_t(cls) << 30);
  }
};

struct RenderTarget {
  RtKey key;
  TrackedImage image;
  // Views by host format (color) or the depth/stencil view.
  std::unordered_map<VkFormat, VkImageView> views;
  uint64_t last_used_frame = 0;
  // Order of GetColor calls, and the guest format of the latest one: the
  // 32bpp and 64bpp images at one key alias the same EDRAM.
  uint64_t use_serial = 0;
  xenos::ColorRenderTargetFormat last_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
};

struct ResolvedSurface {
  uint32_t address = 0;  // physical
  uint32_t pitch = 0, height = 0;  // in pixels (RB_COPY_DEST_PITCH)
  xenos::ColorFormat format = xenos::ColorFormat::k_8_8_8_8;
  // A depth resolve: R32_SFLOAT holding the guest depth value (24-bit unorm
  // or 20e4 float, as the k_24_8 / k_24_8_FLOAT texture decoders produce).
  bool depth = false;
  TrackedImage image;
  VkImageView view = VK_NULL_HANDLE;
  uint64_t last_write_frame = 0;
  uint64_t write_count = 0;
};

class RenderTargetPool {
 public:
  bool Initialize(const rex::ui::vulkan::VulkanDevice* device);
  void Shutdown();

  // Host format of a guest color render-target format; its size class decides
  // which image (32 or 64 bits per host texel) holds it.
  static VkFormat HostColorFormat(xenos::ColorRenderTargetFormat format);
  static RtClass HostColorClass(xenos::ColorRenderTargetFormat format);
  VkFormat depth_format() const { return depth_format_; }

  // Finds or creates the render target (new ones are cleared to zero in `cmd`).
  // 7e3 is 32bpp in EDRAM but hosted in the 64bpp image, so its bits are
  // shared with the 32bpp image at the same key: when the other one of the
  // pair was used more recently with contents this access must see,
  // `*convert_from` is set to it (the caller converts it into the result).
  RenderTarget* GetColor(VkCommandBuffer cmd, uint32_t base_tiles, uint32_t pitch_px,
                         xenos::MsaaSamples msaa, xenos::ColorRenderTargetFormat format,
                         uint64_t frame, RenderTarget** convert_from = nullptr);
  static bool IsFloat7e3(xenos::ColorRenderTargetFormat format) {
    return format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
           format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16;
  }
  RenderTarget* GetDepth(VkCommandBuffer cmd, uint32_t base_tiles, uint32_t pitch_px,
                         xenos::MsaaSamples msaa, uint64_t frame);

  VkImageView GetView(RenderTarget& rt, VkFormat format);
  // Depth-aspect-only view of a depth target, for sampling it.
  VkImageView GetDepthSampleView(RenderTarget& rt);

  // Other existing targets of the same class at the same base whose EDRAM
  // layout matches `rt` (same samples per row), with `rect` converted into
  // their pixels. D3D clears with 4x MSAA at half pitch, for example, which
  // covers the same tiles as the 1x full-pitch surface drawn afterwards.
  struct Alias {
    RenderTarget* rt;
    VkRect2D rect;
  };
  std::vector<Alias> Aliases(const RenderTarget& rt, const VkRect2D& rect);
  // `rt` now holds the latest data of its EDRAM as `format` (written through
  // an alias): GetColor's 32bpp <-> 64bpp conversions take it as the newest.
  void MarkWritten(RenderTarget& rt, xenos::ColorRenderTargetFormat format) {
    rt.use_serial = ++use_serial_;
    rt.last_format = format;
  }

  // Clears `rect` (in pixels) of the render target, with the guest clear
  // value interpreted as `format` (RB_COLOR_CLEAR | RB_COLOR_CLEAR_LO << 32).
  void ClearColor(VkCommandBuffer cmd, RenderTarget& rt, xenos::ColorRenderTargetFormat format,
                  const VkRect2D& rect, uint64_t clear_value);
  // Clears with an already-decoded color (shader output values, in the
  // host view of `format`).
  void ClearColorFloat(VkCommandBuffer cmd, RenderTarget& rt, xenos::ColorRenderTargetFormat format,
                       const VkRect2D& rect, const float color[4]);
  void ClearDepthStencil(VkCommandBuffer cmd, RenderTarget& rt, const VkRect2D& rect,
                         bool clear_depth, float depth, bool clear_stencil, uint32_t stencil);
  // RB_DEPTH_CLEAR: depth in the upper 24 bits, stencil in the low 8.
  void ClearDepth(VkCommandBuffer cmd, RenderTarget& rt, xenos::DepthRenderTargetFormat format,
                  const VkRect2D& rect, uint32_t clear_value);

  size_t size() const { return targets_.size(); }

 private:
  RenderTarget* Create(VkCommandBuffer cmd, const RtKey& key);
  void ClearAttachment(VkCommandBuffer cmd, RenderTarget& rt, VkImageView view,
                       const VkRect2D& rect, const VkClearValue& value,
                       VkImageAspectFlags aspects = 0);

  const rex::ui::vulkan::VulkanDevice* device_ = nullptr;
  VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> targets_;
  uint64_t use_serial_ = 0;
};

class ResolvedSurfaces {
 public:
  void Initialize(const rex::ui::vulkan::VulkanDevice* device) { device_ = device; }
  void Shutdown();

  // Host format a resolve to `format` is stored in, or UNDEFINED if not
  // supported yet.
  static VkFormat HostFormat(xenos::ColorFormat format);

  // Finds or (re)creates the surface at `address` able to hold `pitch` x
  // `height` texels of `format`. Returns nullptr if the format is unsupported.
  // A replaced surface is destroyed once submission `frame` has completed.
  ResolvedSurface* Get(uint32_t address, uint32_t pitch, uint32_t height,
                       xenos::ColorFormat format, uint64_t frame, bool depth = false);
  // The surface last resolved to `address`, if any.
  ResolvedSurface* Find(uint32_t address);

  // Destroys replaced surfaces whose last use is <= `completed_frame`.
  void Collect(uint64_t completed_frame);

  size_t size() const { return surfaces_.size(); }
  template <typename F>
  void ForEach(F&& f) {
    for (auto& [address, surface] : surfaces_) f(*surface);
  }

 private:
  void Destroy(ResolvedSurface& surface);

  const rex::ui::vulkan::VulkanDevice* device_ = nullptr;
  std::unordered_map<uint32_t, std::unique_ptr<ResolvedSurface>> surfaces_;
  std::vector<std::pair<uint64_t, std::unique_ptr<ResolvedSurface>>> retired_;
};

}  // namespace mantra
