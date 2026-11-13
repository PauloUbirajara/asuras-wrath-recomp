#pragma once
#include <chrono>
#include <cstdint>
#include <unordered_set>

#include <rex/graphics/command_processor.h>
#include <rex/ui/vulkan/api.h>

#include "shader_cache.h"

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::native {

class NativeCommandProcessor : public CommandProcessor {
 public:
  NativeCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~NativeCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;
  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  // CPU swap path: the guest frontbuffer is untiled/converted on the CPU into
  // a host-visible staging buffer, then copied into the presenter's guest
  // output image. One upload in flight at a time (fence-synchronized).
  bool EnsureStagingBuffer(VkDeviceSize size);
  void WaitForSwapUpload();
  void DestroySwapResources();
  // Logs a swap summary at most every few seconds (gpu.mantra, info level).
  void ReportSwapStats();

  struct SwapStats {
    uint64_t swaps = 0;
    uint64_t presented = 0;
    uint64_t skipped = 0;
    uint64_t refresh_failed = 0;
    uint32_t last_lit_permille = 0;  // non-black pixels in the last frame, in 1/1000
    uint64_t swaps_at_last_report = 0;
    std::chrono::steady_clock::time_point last_report_time{};
  };
  SwapStats swap_stats_;

  struct FrontbufferDesc {
    uint32_t address = 0, width = 0, height = 0, pitch = 0, format = 0;
    bool tiled = false;
    uint32_t endian = 0, swizzle = 0;
    bool operator==(const FrontbufferDesc&) const = default;
  };
  FrontbufferDesc last_frontbuffer_;

  // Shader capture (see LoadShader / MANTRA_SHADER_DUMP).
  void DumpShader(uint64_t hash, bool is_pixel, const uint32_t* microcode, size_t byte_count);
  struct ShaderStats {
    uint64_t loads = 0;
    uint64_t unique = 0;
    uint64_t dumped = 0;
    // Unique shaders by cache result.
    uint64_t cache_exact = 0;
    uint64_t cache_normalized = 0;
    uint64_t cache_failed = 0;
    uint64_t cache_miss = 0;
  };
  ShaderStats shader_stats_;
  std::unordered_set<uint64_t> shader_hashes_seen_;
  mantra::ShaderCache shader_cache_;

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;

  VkCommandPool swap_command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer swap_command_buffer_ = VK_NULL_HANDLE;
  VkFence swap_fence_ = VK_NULL_HANDLE;
  bool swap_fence_pending_ = false;

  VkBuffer staging_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
  uint32_t staging_memory_type_ = UINT32_MAX;
  VkDeviceSize staging_memory_size_ = 0;
  VkDeviceSize staging_buffer_size_ = 0;
  void* staging_mapping_ = nullptr;
};

}  // namespace rex::graphics::native
