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
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/graphics/xenos.h>

#include "rendering_backend.h"
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
  // Clears deferred until the image is next used (RenderTargetPool::
  // FlushClears): the depth -> color alias clears mostly hit views that are
  // cleared again before anything reads them.
  struct PendingClear {
    VkImageView view;
    VkRect2D rect;  // host pixels
    VkClearValue value;
    VkImageAspectFlags aspects;
  };
  std::vector<PendingClear> pending_clears;
  // Rows [0, dirty_rows) (guest pixels) may differ from the 7e3 <-> 32bpp
  // counterpart: written since the two were last made the same by a
  // conversion. Conversions copy only these rows.
  uint32_t dirty_rows = 0;
  void NoteWritten(uint32_t bottom_row) { dirty_rows = std::max(dirty_rows, bottom_row); }
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
  uint64_t write_count = 0;  // serial of the last resolve into it (0 = never)
  // Set when this surface replaced a smaller one of the same format (a resolve
  // to a larger rect of the same address); the caller copies the old texels
  // over with ResolvedSurfaces::PrepareWrite before writing. Points into the
  // retired list (alive until this frame's submission completes).
  ResolvedSurface* grown_from = nullptr;
  // Cleared to zero before the first write: a resolve may cover only part of
  // the surface, and the rest would otherwise be sampled as garbage.
  bool initialized = false;
  // Views for sampling the surface directly as a texture, by packed swizzle.
  std::unordered_map<uint32_t, VkImageView> sample_views;
};

class RenderTargetPool {
 public:
  bool Initialize(const rex::ui::vulkan::VulkanDevice* device, RenderingBackend* rendering);
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
  // `rscale` (0: ScaleFor): a depth buffer for a pass at another scale (a
  // native HUD pass) is a twin of the scaled one; depth fills clear both.
  RenderTarget* GetDepth(VkCommandBuffer cmd, uint32_t base_tiles, uint32_t pitch_px,
                         xenos::MsaaSamples msaa, uint64_t frame, uint32_t rscale = 0);

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
  // `cls` selects the class of the aliases (default: `rt`'s own). Depth and
  // 32bpp color share the tile layout, so a depth clear can be applied to
  // the 32bpp color targets over the same tiles (kColor32 for a kDepth rt).
  std::vector<Alias> Aliases(const RenderTarget& rt, const VkRect2D& rect,
                             std::optional<RtClass> cls = std::nullopt);
  // `rt` now holds the latest data of its EDRAM as `format` (written through
  // an alias): GetColor's 32bpp <-> 64bpp conversions take it as the newest.
  void MarkWritten(RenderTarget& rt, xenos::ColorRenderTargetFormat format) {
    rt.use_serial = ++use_serial_;
    rt.last_format = format;
  }
  // Whether the color target's 32bpp <-> 64bpp counterpart (same base, pitch
  // and samples) was used more recently, i.e. holds the newer EDRAM data.
  bool CounterpartIsNewer(const RenderTarget& rt) const {
    if (rt.key.cls == RtClass::kDepth) return false;
    RtKey other_key = rt.key;
    other_key.cls = rt.key.cls == RtClass::kColor32 ? RtClass::kColor64 : RtClass::kColor32;
    const auto it = targets_.find(other_key.Hash());
    return it != targets_.end() && it->second->use_serial > rt.use_serial;
  }

  // Clears `rect` (in pixels) of the render target, with the guest clear
  // value interpreted as `format` (RB_COLOR_CLEAR | RB_COLOR_CLEAR_LO << 32).
  // `defer`: record the clear and apply it when the target is next looked
  // up (GetColor / GetDepth) or cleared right away (for alias clears).
  void ClearColor(VkCommandBuffer cmd, RenderTarget& rt, xenos::ColorRenderTargetFormat format,
                  const VkRect2D& rect, uint64_t clear_value, bool defer = false);
  // Clears with an already-decoded color (shader output values, in the
  // host view of `format`).
  void ClearColorFloat(VkCommandBuffer cmd, RenderTarget& rt, xenos::ColorRenderTargetFormat format,
                       const VkRect2D& rect, const float color[4], bool defer = false);
  void ClearDepthStencil(VkCommandBuffer cmd, RenderTarget& rt, const VkRect2D& rect,
                         bool clear_depth, float depth, bool clear_stencil, uint32_t stencil,
                         bool defer = false);
  // Applies the target's deferred clears.
  void FlushClears(VkCommandBuffer cmd, RenderTarget& rt);
  // MANTRA_LAZY_ALIAS_CLEARS=0 applies alias clears right away.
  static bool DeferAliasClears();
  uint64_t deferred_clears() const { return deferred_clears_; }
  uint64_t flushed_clears() const { return flushed_clears_; }
  // RB_DEPTH_CLEAR: depth in the upper 24 bits, stencil in the low 8.
  void ClearDepth(VkCommandBuffer cmd, RenderTarget& rt, xenos::DepthRenderTargetFormat format,
                  const VkRect2D& rect, uint32_t clear_value);

  size_t size() const { return targets_.size(); }

  // Native HUD (render scale below 1x): the 32bpp target the game resolves
  // to the frontbuffer (where it draws the HUD over the tonemapped scene)
  // stays at the guest resolution; the rest uses the render scale.
  // MANTRA_NATIVE_HUD=0 scales everything.
  static bool NativeHud();
  // A target resolved to the frontbuffer: native from its next lookup on.
  void SetPresentation(const RtKey& key);
  // The native twin of a scaled depth buffer, if one was made.
  RenderTarget* DepthTwin(const RenderTarget& rt);
  uint32_t ScaleFor(const RtKey& key) const;
  // Frees targets replaced at another scale once `completed_frame` is done.
  void Collect(uint64_t completed_frame);

 private:
  RenderTarget* Create(VkCommandBuffer cmd, const RtKey& key, bool twin = false);
  void Destroy(RenderTarget& rt);
  void ClearAttachment(VkCommandBuffer cmd, RenderTarget& rt, VkImageView view,
                       const VkRect2D& rect, const VkClearValue& value,
                       VkImageAspectFlags aspects = 0, bool defer = false);

  RenderingBackend* rendering_ = nullptr;
  const rex::ui::vulkan::VulkanDevice* device_ = nullptr;
  VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> targets_;
  uint64_t use_serial_ = 0;
  uint64_t deferred_clears_ = 0, flushed_clears_ = 0;
  std::unordered_set<uint64_t> presentation_keys_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> depth_twins_;  // native scale
  std::vector<std::pair<uint64_t, std::unique_ptr<RenderTarget>>> retired_;
};

class ResolvedSurfaces {
 public:
  void Initialize(const rex::ui::vulkan::VulkanDevice* device);
  void Shutdown();

  // Host format a resolve to `format` is stored in, or UNDEFINED if not
  // supported yet.
  static VkFormat HostFormat(xenos::ColorFormat format);

  // Finds or (re)creates the surface at `address` able to hold `pitch` x
  // `height` texels of `format`. Returns nullptr if the format is unsupported.
  // A replaced surface is destroyed once submission `frame` has completed.
  // `rscale`: the image's render scale (RenderScaleUnits() units).
  ResolvedSurface* Get(uint32_t address, uint32_t pitch, uint32_t height,
                       xenos::ColorFormat format, uint64_t frame, bool depth, uint32_t rscale);
  // The surface last resolved to `address`, if any.
  ResolvedSurface* Find(uint32_t address);
  // Call before each write: zeroes a new surface, and if it grew from an older
  // one (see grown_from), copies the old contents in so partial resolves
  // (e.g. one page half after the other) keep what was resolved before.
  void PrepareWrite(VkCommandBuffer cmd, ResolvedSurface& surface);

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
  PFN_vkCmdCopyImage cmd_copy_image_ = nullptr;  // not in the SDK's function table
  std::unordered_map<uint32_t, std::unique_ptr<ResolvedSurface>> surfaces_;
  std::vector<std::pair<uint64_t, std::unique_ptr<ResolvedSurface>>> retired_;
};

}  // namespace mantra
