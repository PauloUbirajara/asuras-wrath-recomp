#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>
#include <rex/ui/vulkan/api.h>

#include "copy_pipelines.h"
#include "render_targets.h"
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
  // --- Submissions ---------------------------------------------------------
  // All GPU work of a guest frame (resolves, clears, the present) is recorded
  // into one command buffer, submitted at the swap. kFrameSlots submissions
  // may be in flight; a slot is reused once its fence has signaled.
  static constexpr uint32_t kFrameSlots = 3;
  // Descriptor sets per slot (one per copy); the frame is submitted early if
  // it runs out.
  static constexpr uint32_t kDescriptorSetsPerSlot = 512;
  struct FrameSlot {
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    uint32_t descriptor_sets_used = 0;
    uint64_t submission = 0;
    bool pending = false;
  };
  FrameSlot& CurrentSlot() { return frame_slots_[submission_current_ % kFrameSlots]; }
  // Returns the command buffer of the current submission, beginning it if
  // needed (VK_NULL_HANDLE on failure).
  VkCommandBuffer BeginCommands();
  // Ends and submits the current submission, if anything was recorded.
  bool SubmitCommands();
  // Waits until submission `index` has completed.
  void AwaitSubmission(uint64_t index);
  void AwaitSlot(FrameSlot& slot);
  // Reserves a descriptor set slot in the current submission, submitting
  // first if the pool is full. Returns the pool to allocate from.
  VkDescriptorPool ReserveDescriptorSet();
  void DestroyFrameResources();

  // --- Interim draw path ----------------------------------------------------
  // The driver's solid-fill program (D3D Clear and fill rects) executed as
  // host clears (solid_fill.cpp).
  enum class SolidFillResult { kNotSolidFill, kDone, kUnsupported };
  SolidFillResult TrySolidFill(xenos::PrimitiveType prim_type, xenos::EdramMode edram_mode);

  // --- EDRAM / resolves -----------------------------------------------------
  // Executes the resolve set up in the RB_COPY_* registers: copy an EDRAM
  // rectangle to a resolved surface and/or clear it.
  bool Resolve();

  // --- Swap -----------------------------------------------------------------
  // GPU present: draws a resolved surface into the presenter's guest output.
  bool PresentResolvedSurface(mantra::ResolvedSurface& surface, uint32_t width, uint32_t height,
                              uint32_t swizzle, bool is_8bpc);
  // CPU fallback: untile/convert the guest frontbuffer from memory.
  bool PresentFromGuestMemory(const uint8_t* src, uint32_t width, uint32_t height, uint32_t pitch,
                              bool tiled, xenos::TextureFormat format, xenos::Endian endian,
                              uint32_t swizzle, bool is_8bpc);
  bool EnsureStagingBuffer(VkDeviceSize size);
  void DestroySwapResources();
  // MANTRA_CAPTURE_DIR=<dir>: every 150th swap presented from a resolved
  // surface is read back and written as <dir>/frame_<swap>.ppm (debug aid).
  void CaptureSurface(mantra::ResolvedSurface& surface, uint32_t width, uint32_t height,
                      uint32_t swizzle, const std::string& name);
  // Logs a swap summary at most every few seconds (gpu.mantra, info level).
  void ReportSwapStats();

  struct SwapStats {
    uint64_t swaps = 0;
    uint64_t presented = 0;
    uint64_t presented_gpu = 0;  // from a resolved surface
    uint64_t presented_cpu = 0;  // from guest memory
    uint64_t skipped = 0;
    uint64_t refresh_failed = 0;
    uint32_t last_lit_permille = 0;  // non-black pixels in the last frame, in 1/1000
    uint64_t swaps_at_last_report = 0;
    uint64_t reports = 0;
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
  bool last_present_gpu_ = false;

  struct DrawStats {
    uint64_t draws = 0;
    uint64_t copy_draws = 0;  // edram_mode kCopy, executed as resolves
    uint64_t resolves_failed = 0;
    uint64_t color_copies = 0;
    uint64_t depth_copies_skipped = 0;
    uint64_t unsupported_dest_format = 0;
    uint64_t color_clears = 0;
    uint64_t depth_clears = 0;
    uint64_t fills_color = 0;  // solid fills executed as clears
    uint64_t fills_depth = 0;
    uint64_t fills_skipped = 0;
  };
  DrawStats draw_stats_;
  // Distinct fill / resolve routes already logged (debug).
  std::unordered_map<uint64_t, uint64_t> logged_routes_;
  bool FirstRoute(uint64_t key) { return logged_routes_.emplace(key, 0).second; }

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
  // Loaded shaders by microcode hash, [vertex, pixel].
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_[2];
  string::StringBuffer ucode_disasm_buffer_;

  // Draw census: (vs hash, ps hash, edram mode | prim << 8) -> count.
  std::map<std::tuple<uint64_t, uint64_t, uint32_t>, uint64_t> draw_census_;
  void ReportDrawCensus();
  mantra::ShaderCache shader_cache_;

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;

  FrameSlot frame_slots_[kFrameSlots];
  uint64_t submission_current_ = 1;  // the one being recorded
  uint64_t submission_completed_ = 0;
  bool recording_ = false;

  mantra::CopyPipelines copy_pipelines_;
  mantra::RenderTargetPool render_targets_;
  mantra::ResolvedSurfaces resolved_surfaces_;

  // CPU swap path staging (last used by staging_submission_).
  uint64_t staging_submission_ = 0;

  VkBuffer staging_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
  uint32_t staging_memory_type_ = UINT32_MAX;
  VkDeviceSize staging_memory_size_ = 0;
  VkDeviceSize staging_buffer_size_ = 0;
  void* staging_mapping_ = nullptr;
};

}  // namespace rex::graphics::native
