#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>
#include <rex/ui/vulkan/api.h>

#include "copy_pipelines.h"
#include "draw_pipelines.h"
#include "render_targets.h"
#include "shader_cache.h"
#include "shader_interface.h"

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::native {

// A guest shader plus its offline translation (if the cache has one).
class MantraShader : public Shader {
 public:
  using Shader::Shader;
  const mantra::shader_cache::CacheEntry* cache_entry = nullptr;
  VkShaderModule module = VK_NULL_HANDLE;

  // Vertex shaders: the translated shader's inputs, with the vfetch each one
  // comes from decoded from this (possibly driver-patched) microcode.
  struct VertexInput {
    uint32_t location = 0;
    mantra::shader_cache::DeclUsage usage = mantra::shader_cache::DeclUsage::kPosition;
    bool is_uint = false;  // declared uint4
    bool valid = false;    // the vfetch was found and decoded
    uint32_t fetch_constant = 0;
    uint32_t stride_words = 0;
    int32_t offset_words = 0;
    xenos::VertexFormat format = xenos::VertexFormat::kUndefined;
    bool is_signed = false;
    bool normalized = true;
  };
  std::vector<VertexInput> vertex_inputs;
  // Pixel shaders: bits 0..3 = writes oC0..3, bit 4 = writes oDepth.
  uint32_t ps_outputs = 0;

  void DecodeVertexInputs(std::span<const uint32_t> packed_elements);
};

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

  // --- Guest draws (guest_draw.cpp) ----------------------------------------
  enum class DrawResult { kDrawn, kSkipped, kFailed };
  // Draws with the translated shaders and a pipeline from the guest state.
  DrawResult DrawGuest(xenos::PrimitiveType prim_type, uint32_t index_count,
                       IndexBufferInfo* index_buffer_info);
  // Uploads this draw's vertex (and index) data into the ring; fills the
  // pipeline key's vertex input and the draw parameters.
  struct GeometryUpload {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize binding_offsets[mantra::kMaxVertexBindings] = {};
    uint32_t binding_count = 0;
    bool indexed = false;
    uint32_t index_offset = 0;  // bytes in the ring
    VkIndexType index_type = VK_INDEX_TYPE_UINT32;
    uint32_t draw_count = 0;    // vertices or indices
    int32_t vertex_offset = 0;  // added to indices (vkCmdDrawIndexed)
  };
  DrawResult UploadGeometry(const MantraShader& vs, xenos::PrimitiveType prim_type,
                            uint32_t index_count, IndexBufferInfo* index_buffer_info,
                            bool restart_allowed, mantra::PipelineKey& key,
                            GeometryUpload& upload, uint32_t& spec_constants);
  // Begins (or keeps) dynamic rendering into this draw's render targets.
  // Returns false if no target could be bound.
  bool BeginDrawRendering(VkCommandBuffer cmd, uint32_t color_mask, bool use_depth,
                          mantra::PipelineKey& key, VkRect2D& render_area);
  // Ends the open dynamic rendering, if any; required before any other
  // command (transfers, barriers, clears, copies).
  void EndRendering();
  // BeginCommands + EndRendering, for everything that is not a guest draw.
  VkCommandBuffer BeginNonDrawCommands();

  struct ActiveRendering {
    bool active = false;
    mantra::RenderTarget* color[mantra::kMaxColorAttachments] = {};
    VkFormat color_format[mantra::kMaxColorAttachments] = {};
    mantra::RenderTarget* depth = nullptr;
    VkRect2D area = {};
  };
  ActiveRendering rendering_;

  struct GuestDrawStats {
    uint64_t drawn = 0;
    uint64_t skipped_primitive = 0;  // unsupported primitive type
    uint64_t skipped_no_target = 0;
    uint64_t skipped_viewport = 0;   // empty viewport / scissor
    uint64_t skipped_geometry = 0;   // vertex/index data unavailable
    uint64_t failed_pipeline = 0;
    uint64_t missing_inputs = 0;     // inputs fed zeros (undecodable fetch / format)
    uint64_t render_passes = 0;
    uint64_t vertex_bytes = 0;
    uint64_t index_bytes = 0;
  };
  GuestDrawStats guest_draw_stats_;

  // --- Guest shader constants -------------------------------------------------
  // Uploads this draw's VS/PS float constants and shared constants into the
  // constant ring (reusing the previous upload of this submission when the
  // registers did not change). `shared` provides the draw-specific shared
  // constants (viewport transform); booleans and the alpha threshold are
  // filled here. Returns false if nothing could be uploaded.
  bool PrepareDrawConstants(mantra::DrawConstantOffsets& offsets,
                            const mantra::SharedConstants& shared);
  bool UploadConstants(const void* data, uint32_t size, uint32_t& offset);
  void DestroyShaderModules();

  struct ConstantCache {
    uint64_t submission = 0;  // submission the cached upload belongs to
    uint32_t offset = 0;
    std::vector<uint8_t> data;
  };
  ConstantCache vs_constants_, ps_constants_, shared_constants_;

  struct ConstantStats {
    uint64_t draws = 0;          // draws whose constants were prepared
    uint64_t uploads = 0;        // ring copies
    uint64_t reused = 0;         // stages that reused the previous upload
    uint64_t bytes = 0;
    uint64_t ring_full = 0;      // early submits because the ring filled up
    uint64_t no_translation = 0; // draws with a shader lacking SPIR-V
  };
  ConstantStats constant_stats_;

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
    uint64_t modules = 0;          // VkShaderModules created
    uint64_t module_failures = 0;
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
  mantra::DrawPipelines draw_pipelines_;
  mantra::ShaderInterface shader_interface_;
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
