#pragma once
// Guest textures for the translated shaders.
//
// A texture is identified by the parts of its fetch constant that describe
// the data (address, size, format, tiling, mips); sampler state and the
// swizzle are applied separately (VkSampler, image view component mapping).
//
// Loading: the guest data is copied with the fetch constant's endian swap,
// untiled on the CPU (texture_util's guest layout and tiled addressing, the
// same math as the SDK's texture cache) into ring staging space, and copied
// into a device-local image. The guest pages are write-watched through the
// buffer cache; a CPU write makes the next use reload the texture in place.
//
// Resolve destinations are rendered on the GPU (ResolvedSurfaces) and never
// written back to guest memory, so a texture at a resolve address samples the
// resolved surface instead, unless the CPU wrote the memory after the resolve.
//
// Formats Vulkan has no match for are decoded on the CPU while untiling
// (CTX1, DXT3A, 10:11:11 / 11:11:10, 24:8 depth, 4:2:2 YUV). Stacked 2D
// textures only use layer 0: the translated shaders sample Texture2D.
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace mantra {

class BufferCache;
class ResolvedSurfaces;

class TextureCache {
 public:
  // What the cache needs from the command processor while loading.
  class Host {
   public:
    // Staging space in the upload ring; may submit to make room (so fetch the
    // command buffer only afterwards). nullptr on failure.
    virtual uint8_t* AllocateStaging(uint32_t size, VkBuffer& buffer, VkDeviceSize& offset) = 0;
    // The current command buffer, outside dynamic rendering.
    virtual VkCommandBuffer BeginTransferCommands() = 0;
    virtual uint64_t CurrentSubmission() const = 0;

   protected:
    ~Host() = default;
  };

  enum Dimension : uint32_t { k2D = 0, k3D = 1, kCube = 2 };
  struct Binding {
    VkImageView view = VK_NULL_HANDLE;  // null: use the placeholder
    VkSampler sampler = VK_NULL_HANDLE;
    Dimension dimension = k2D;
  };

  struct Stats {
    uint64_t loads = 0;           // textures (re)loaded from guest memory
    uint64_t reloads = 0;         // of those, after a CPU write
    uint64_t load_bytes = 0;      // guest bytes read
    uint64_t resolved_binds = 0;  // bound from a resolved surface
    uint64_t resolve_mismatches = 0;  // resolved surface not copyable (format): memory used
    uint64_t unsupported = 0;     // format/dimension not handled (placeholder)
    uint64_t failures = 0;        // creation/staging failures (placeholder)
  };

  bool Initialize(const rex::ui::vulkan::VulkanDevice* device, BufferCache* watcher,
                  ResolvedSurfaces* resolved);
  void Shutdown();

  // The view and sampler for a texture fetch constant, loading it if needed.
  Binding Get(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch, Host& host);

  // A resolve wrote `address` on the GPU: textures there use the resolved
  // surface until the CPU writes the memory.
  void NoteResolve(uint32_t address, uint32_t length);

  const Stats& stats() const { return stats_; }
  size_t size() const { return textures_.size(); }
  uint32_t sampler_count() const { return uint32_t(samplers_.size()); }

 private:
  struct Key {
    uint32_t words[6];
    bool operator==(const Key& other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
  };
  struct KeyHash {
    size_t operator()(const Key& key) const;
  };
  struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    Dimension dimension = k2D;
    uint32_t width = 0, height = 0, depth = 1, layers = 1, levels = 1;
    uint8_t decode = 0;     // how guest blocks become host texels
    uint8_t host_bpb = 0;   // bytes per host block
    uint32_t dump_counter = 0;
    // Host channel holding each guest component (X, Y, Z, W), as
    // VkComponentSwizzle values; components past `component_count` repeat
    // the last one.
    uint8_t channels[4] = {};
    uint32_t component_count = 4;
    bool initialized = false;  // has contents (SHADER_READ_ONLY_OPTIMAL)
    bool unsupported = false;
    // Guest data watched for CPU writes.
    uint32_t base_start = 0, base_length = 0, mip_start = 0, mip_length = 0;
    uint32_t epoch = 0, base_checked = 0, mip_checked = 0;
    // When copied from a resolved surface: which one, and its write count.
    VkImage resolved_image = VK_NULL_HANDLE;
    uint64_t resolved_writes = 0;
    std::unordered_map<uint32_t, VkImageView> views;  // by packed swizzle
  };
  struct ResolveRecord {
    uint32_t length = 0;
    uint32_t epoch = 0;
    uint32_t checked = 0;
  };

  bool CreateImage(Texture& texture);
  bool LoadFromMemory(Texture& texture, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                      Host& host);
  bool CopyFromResolved(Texture& texture, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
                        uint32_t address, Host& host);
  VkImageView GetView(Texture& texture, uint32_t swizzle);
  VkSampler GetSampler(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);
  // The resolved surface backing `address`, if still newer than the memory.
  bool ResolvedValid(uint32_t address);

  const rex::ui::vulkan::VulkanDevice* device_ = nullptr;
  BufferCache* watcher_ = nullptr;
  PFN_vkCmdCopyImage cmd_copy_image_ = nullptr;
  ResolvedSurfaces* resolved_ = nullptr;
  std::unordered_map<Key, std::unique_ptr<Texture>, KeyHash> textures_;
  std::unordered_map<uint64_t, VkSampler> samplers_;
  std::unordered_map<uint32_t, ResolveRecord> resolves_;
  std::vector<uint8_t> swapped_base_, swapped_mips_;
  uint64_t unsupported_logged_ = 0;  // guest formats already reported
  Stats stats_;
};

}  // namespace mantra
