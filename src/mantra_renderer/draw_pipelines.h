#pragma once
// Graphics pipelines for guest draws: the translated (cached) shaders plus the
// fixed-function state taken from the guest registers, rendering with
// dynamic rendering into the RenderTargetPool images.
//
// Dynamic state: viewport, scissor, depth bias, blend constants, stencil
// compare/write masks and reference.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace mantra {

inline constexpr uint32_t kMaxVertexBindings = 16;
inline constexpr uint32_t kMaxVertexAttributes = 32;
inline constexpr uint32_t kMaxColorAttachments = 4;

// Everything a pipeline depends on. Compared and hashed bytewise: always
// zero-initialize (padding included) before filling.
struct PipelineKey {
  uint64_t vertex_module;  // VkShaderModule
  uint64_t pixel_module;   // VkShaderModule, 0 = no fragment shader
  uint32_t spec_constants;
  uint32_t depth_format;  // VkFormat, 0 = no depth/stencil attachment
  uint32_t color_formats[kMaxColorAttachments];  // VkFormat, 0 = unused
  uint8_t color_count;    // attachments up to the last used one
  uint8_t sample_count;   // VkSampleCountFlagBits
  uint8_t topology;       // VkPrimitiveTopology
  uint8_t rect_list;      // expand rectangles in the geometry shader
  uint8_t primitive_restart;
  uint8_t polygon_mode;   // VkPolygonMode
  uint8_t cull_mode;      // VkCullModeFlags
  uint8_t front_face;     // VkFrontFace
  uint8_t depth_bias;
  uint8_t depth_test;
  uint8_t depth_write;
  uint8_t depth_compare;  // VkCompareOp
  uint8_t stencil_test;
  uint8_t stencil_front[4];  // fail, pass, depth fail (VkStencilOp), compare (VkCompareOp)
  uint8_t stencil_back[4];
  uint8_t alpha_to_coverage;
  struct Blend {
    uint8_t enable;
    uint8_t src_color, dst_color, color_op;  // VkBlendFactor / VkBlendOp
    uint8_t src_alpha, dst_alpha, alpha_op;
    uint8_t write_mask;  // VkColorComponentFlags
  } blend[kMaxColorAttachments];
  uint8_t binding_count;
  uint8_t attribute_count;
  uint8_t pad[2];
  uint32_t binding_strides[kMaxVertexBindings];  // bytes
  struct Attribute {
    uint8_t location;
    uint8_t binding;
    uint16_t offset;  // bytes
    uint32_t format;  // VkFormat
  } attributes[kMaxVertexAttributes];

  bool operator==(const PipelineKey& other) const {
    return std::memcmp(this, &other, sizeof(*this)) == 0;
  }
};

class DrawPipelines {
 public:
  bool Initialize(const rex::ui::vulkan::VulkanDevice* device, VkPipelineLayout layout);
  void Shutdown();

  // Finds or creates the pipeline (VK_NULL_HANDLE if creation failed; failures
  // are remembered). Called from the GPU thread only.
  //
  // With `pending` (and async creation on, MANTRA_ASYNC_PIPELINES != 0), a
  // pipeline that isn't built yet is queued for the background threads
  // instead: Get returns VK_NULL_HANDLE with *pending = true and the caller
  // skips the draw. Building one takes 30-70 ms on integrated GPUs, which
  // otherwise stalls the frame.
  VkPipeline Get(const PipelineKey& key, bool* pending = nullptr);

  // Driver pipeline cache kept on disk at `path` (loaded now, saved by
  // SaveCache and at shutdown). Call before WarmUp and the first Get.
  void OpenCache(const std::filesystem::path& path);
  // Saves the pipeline cache in the background if pipelines were created
  // since the last save.
  void SaveCacheIfChanged();

  // Creates `keys` on background threads; Get takes them when they are asked
  // for (or creates them itself if not ready yet).
  void WarmUp(std::vector<PipelineKey> keys);

  size_t size() const { return pipelines_.size(); }
  uint64_t async_created() const { return async_created_; }  // taken from the async queue
  uint64_t pending_skips() const { return pending_skips_; }  // Gets that returned pending
  uint64_t created() const { return created_; }           // by Get, on the GPU thread
  uint64_t warm_hits() const { return warm_hits_; }       // Gets served by the warm-up
  size_t warm_total() const { return warm_keys_.size(); }
  size_t warm_done() const { return warm_done_.load(std::memory_order_relaxed); }
  uint64_t creation_failures() const { return creation_failures_; }
  bool rect_lists_supported() const { return rect_list_gs_ != VK_NULL_HANDLE; }

 private:
  VkPipeline Create(const PipelineKey& key);

  struct KeyHash {
    size_t operator()(const PipelineKey& key) const;
  };

  const rex::ui::vulkan::VulkanDevice* device_ = nullptr;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkShaderModule rect_list_gs_ = VK_NULL_HANDLE;
  std::unordered_map<PipelineKey, VkPipeline, KeyHash> pipelines_;
  uint64_t creation_failures_ = 0;
  uint64_t created_ = 0;

  // Pipeline cache.
  VkPipelineCache cache_ = VK_NULL_HANDLE;
  std::filesystem::path cache_path_;
  PFN_vkDestroyPipelineCache destroy_cache_ = nullptr;
  PFN_vkGetPipelineCacheData get_cache_data_ = nullptr;
  std::atomic<uint64_t> cache_changes_{0};  // pipelines created (any thread)
  uint64_t cache_saved_changes_ = 0;
  std::thread save_thread_;
  void SaveCache();

  // Warm-up.
  void WarmWorker();
  void StopWarmUp();
  std::vector<PipelineKey> warm_keys_;
  std::atomic<size_t> warm_next_{0}, warm_done_{0};
  std::atomic<bool> warm_stop_{false};
  std::vector<std::thread> warm_threads_;
  std::mutex warm_mutex_;
  std::unordered_map<PipelineKey, VkPipeline, KeyHash> warm_ready_;  // under warm_mutex_
  std::chrono::steady_clock::time_point warm_start_;
  uint64_t warm_hits_ = 0;

  // Async creation of pipelines first asked for in game.
  void AsyncWorker();
  bool async_enabled_ = true;
  std::deque<PipelineKey> async_queue_;  // under warm_mutex_
  std::unordered_map<PipelineKey, char, KeyHash> async_pending_;  // GPU thread only
  std::condition_variable async_cv_;
  std::vector<std::thread> async_threads_;
  bool async_stop_ = false;  // under warm_mutex_
  uint64_t async_created_ = 0;
  uint64_t pending_skips_ = 0;
};

}  // namespace mantra
