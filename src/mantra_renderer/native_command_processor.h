#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>
#include <rex/ui/vulkan/api.h>

#include "buffer_cache.h"
#include "memexport.h"
#include "copy_pipelines.h"
#include "draw_pipelines.h"
#include "pipeline_store.h"
#include "render_targets.h"
#include "shader_cache.h"
#include "shader_interface.h"
#include "texture_cache.h"

namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace rex::graphics::native {

// Guest swaps (IssueSwap calls) since start.
inline std::atomic<uint64_t> g_guest_swaps{0};
// Whether a traced frame also saves the presented picture next to its log
// (<trace>.png): -1 = not set yet ($MANTRA_TRACE_CAPTURE, default on), else
// set by the app (Settings > Frame traces) through mantra_set_trace_capture.
inline std::atomic<int> g_trace_capture{-1};


// A guest shader plus its offline translation (if the cache has one).
class MantraShader : public Shader {
 public:
  using Shader::Shader;
  // A copy: the cache is replaced when it is rebuilt while the game runs.
  std::optional<mantra::shader_cache::CacheEntry> cache_entry;
  VkShaderModule module = VK_NULL_HANDLE;
  // The cache had no exact translation: the big-endian microcode, to look the
  // shader up again when the cache is rebuilt (ReloadShaderCacheIfChanged).
  std::vector<uint32_t> ucode_be;

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
    int32_t exp_adjust = 0;  // results * 2^exp_adjust
    // Non-zero when the runtime fetch's destination swizzle differs from the
    // container's the translation reads the input with: the input is rebuilt
    // as SharedConstants::input_remap describes (12 bits; 0 = as fetched).
    uint32_t remap = 0;
    // Whether the translation decodes this input as packed 10:11:11 (its
    // container fetch's format; cache v10), or -1 if unknown (then taken to
    // be the runtime format's).
    int8_t container_packed = -1;
  };
  std::vector<VertexInput> vertex_inputs;
  std::string vfetch_debug;  // all vfetches + cache elements (trace)
  // Pixel shaders: bits 0..3 = writes oC0..3, bit 4 = writes oDepth.
  uint32_t ps_outputs = 0;
  // Vertex fetches read element floor(vertex index / this) when > 1.
  uint32_t vertex_index_divisor = 0;

  // `container_swizzles`: per element, its fetch's destination swizzle in the
  // translated microcode (empty for caches before v7), with (v10,
  // `container_formats`) shader_cache::kSwizzlePackedFormat.
  void DecodeVertexInputs(std::span<const uint32_t> packed_elements,
                          std::span<const uint32_t> container_swizzles,
                          bool container_formats = false);
};

class NativeCommandProcessor : public CommandProcessor, private mantra::TextureCache::Host {
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
  // Debug: log pixel shader float constant writes in traced frames.
  void WriteRegister(uint32_t index, uint32_t value) override;
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                             uint32_t num_registers) override;

 private:
  bool in_register_range_write_ = false;
  void TracePsConstantWrite(uint32_t start_index, const uint32_t* values, uint32_t count,
                            const void* source);
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

  // RenderTargetPool::GetColor, plus the 7e3 <-> 32bpp conversion it may ask
  // for, recorded into `cmd` (outside rendering; never submits).
  mantra::RenderTarget* GetColorTarget(VkCommandBuffer cmd, uint32_t base_tiles,
                                       uint32_t pitch_px, xenos::MsaaSamples msaa,
                                       xenos::ColorRenderTargetFormat format, uint64_t frame);
  void DestroyFrameResources();

  // --- Interim draw path ----------------------------------------------------
  // The driver's solid-fill program (D3D Clear and fill rects) executed as
  // host clears (solid_fill.cpp).
  enum class SolidFillResult { kNotSolidFill, kDone, kUnsupported };
  SolidFillResult TrySolidFill(xenos::PrimitiveType prim_type, xenos::EdramMode edram_mode,
                               uint32_t vertex_count, bool indexed);
  SolidFillResult TrySolidFillRect(xenos::PrimitiveType prim_type, xenos::EdramMode edram_mode,
                                   uint32_t first_vertex);

  // --- Guest draws (guest_draw.cpp) ----------------------------------------
  enum class DrawResult { kDrawn, kSkipped, kFailed };
  // Debug tracing (MANTRA_TRACE_SWAP / MANTRA_TRACE_VS): whether the current
  // frame's draws are logged.
  bool TracingThisFrame();
  void MaybeTraceOnResolve(uint32_t address, uint32_t width, uint32_t height);
  uint64_t trace_swap_ = 0;
  std::filesystem::path trace_file_;  // the traced frame's lines (mantra::TraceLine)
  bool trace_from_trigger_ = false;  // trace_swap_ came from MANTRA_TRACE_TRIGGER
  // Serial of the last resolve; a resolved surface's write_count is the
  // serial of its last write (unique across recreated surfaces).
  uint64_t resolve_serial_ = 0;
  void TraceDraw(xenos::PrimitiveType prim_type, uint32_t index_count, const char* outcome);
  // While tracing with MANTRA_CAPTURE_DIR: writes the draw's guest index and
  // vertex buffers (raw, as in memory) for offline geometry checks.
  void DumpDrawBuffers(uint32_t index_count, const IndexBufferInfo* index_buffer_info);
  // Draws with the translated shaders and a pipeline from the guest state.
  DrawResult DrawGuest(xenos::PrimitiveType prim_type, uint32_t index_count,
                       IndexBufferInfo* index_buffer_info);
  // Uploads this draw's vertex (and index) data into the ring; fills the
  // pipeline key's vertex input and the draw parameters.
  struct GeometryUpload {
    VkBuffer binding_buffers[mantra::kMaxVertexBindings] = {};
    VkDeviceSize binding_offsets[mantra::kMaxVertexBindings] = {};
    uint32_t binding_count = 0;
    bool indexed = false;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT32;
    uint32_t draw_count = 0;    // vertices or indices
    int32_t vertex_offset = 0;  // added to indices (vkCmdDrawIndexed)
    uint32_t vertex_index_base = 0;  // guest index of host vertex index 0
  };
  struct IndexRange {
    uint32_t min = 0, max = 0;  // referenced vertices (reset indices excluded)
    bool restart = false;       // host primitive restart (0xFFFFFFFF)
  };
  // Converts (or finds in the buffer cache) the draw's indices; leaves
  // upload.indexed false for non-indexed draws that need no conversion.
  DrawResult UploadIndices(xenos::PrimitiveType prim_type, uint32_t index_count,
                           IndexBufferInfo* index_buffer_info, bool restart_allowed,
                           GeometryUpload& upload, IndexRange& range);
  // Vertex data [start, start + size) of the fetch buffer from the buffer
  // cache; false if it can't be cached (the caller copies it into the ring).
  bool CachedVertexData(uint32_t buffer_address, uint32_t buffer_size, xenos::Endian endian,
                        uint32_t start, uint32_t size, VkBuffer& buffer, VkDeviceSize& offset);
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

  // Textures: per fetch slot, the view in its dimension's heap and the
  // sampler (null = placeholder).
  struct TextureBindings {
    VkImageView views[3][mantra::ShaderInterface::kFetchSlots];
    VkSampler samplers[mantra::ShaderInterface::kFetchSlots];
    int32_t exp_bias[mantra::ShaderInterface::kFetchSlots];
    bool operator==(const TextureBindings& other) const {
      return std::memcmp(this, &other, sizeof(*this)) == 0;
    }
  };
  // Loads the textures the draw's shaders sample (may record transfers and
  // submit, so call before anything else of the draw is recorded).
  void PrepareTextures(const MantraShader* vs, const MantraShader* ps, TextureBindings& out);
  // The descriptor set for `bindings` in the current submission (reused while
  // unchanged); VK_NULL_HANDLE when the slot's pool is exhausted.
  VkDescriptorSet GetTextureSet(const TextureBindings& bindings);
  TextureBindings last_texture_bindings_ = {};
  VkDescriptorSet last_texture_set_ = VK_NULL_HANDLE;
  uint64_t last_texture_set_submission_ = 0;
  // TextureCache::Host.
  uint8_t* AllocateStaging(uint32_t size, VkBuffer& buffer, VkDeviceSize& offset) override;
  VkCommandBuffer BeginTransferCommands() override;
  uint64_t CurrentSubmission() const override { return submission_current_; }

  struct ActiveRendering {
    bool active = false;
    mantra::RenderTarget* color[mantra::kMaxColorAttachments] = {};
    VkFormat color_format[mantra::kMaxColorAttachments] = {};
    mantra::RenderTarget* depth = nullptr;
    VkRect2D area = {};
    uint32_t rscale = 40;  // kScaleOne; of all the attachments
  };
  ActiveRendering rendering_;
  // Render scale a draw with these targets renders at (see
  // RenderTargetPool::ScaleFor): its first color target's, else the scale.
  uint32_t DrawScaleUnits(uint32_t color_mask) const;
  // Depth-only passes render at the scale of the last color pass this frame
  // (a HUD stencil mask after the native tonemap; the scene's depth prepass
  // before it): reset to the render scale at each swap.
  uint32_t last_color_rscale_ = 40;  // kScaleOne
  // Resolved surfaces: native with a native HUD (resolves upscale the scene).
  static uint32_t SurfaceScaleUnits();

  struct GuestDrawStats {
    uint64_t drawn = 0;
    uint64_t skipped_primitive = 0;  // unsupported primitive type
    uint64_t skipped_no_target = 0;
    uint64_t skipped_viewport = 0;   // empty viewport / scissor
    uint64_t skipped_geometry = 0;   // vertex/index data unavailable
    uint64_t failed_pipeline = 0;
    uint64_t skipped_pending_pipeline = 0;  // pipeline still building (async)
    uint64_t missing_inputs = 0;     // inputs fed zeros (undecodable fetch / format)
    uint64_t expanded_inputs = 0;    // inputs decoded on the CPU (10:11:11 into float)
    uint64_t render_passes = 0;
    uint64_t depth_scale_mismatch = 0;  // depth left out (native HUD pass)
    uint64_t vertex_bytes = 0;
    uint64_t expand_ns = 0;          // decoding those inputs
    uint64_t index_bytes = 0;
    uint64_t texture_sets = 0;
    uint64_t texture_sets_reused = 0;
  };
  GuestDrawStats guest_draw_stats_;
  // CPU-decoded vertex inputs are built here (cached memory), then copied
  // into upload memory at once.
  std::vector<float> expanded_scratch_;

  // --- Guest shader constants -------------------------------------------------
  // Uploads this draw's VS/PS float constants and shared constants into the
  // constant ring (reusing the previous upload of this submission when the
  // registers did not change). `shared` provides the draw-specific shared
  // constants (viewport transform); booleans and the alpha threshold are
  // filled here. Returns false if nothing could be uploaded.
  bool PrepareDrawConstants(const MantraShader* vs, const MantraShader* ps,
                            mantra::DrawConstantOffsets& offsets,
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
                      uint32_t swizzle, const std::string& name,
                      const std::filesystem::path& path = {});
  // Reads back a single-sample color image whose texels are `format` and
  // writes <MANTRA_CAPTURE_DIR>/<name>.ppm, logging per-channel mean / max.
  // With `path`, writes that file instead (.png or .ppm by its extension).
  void CaptureImage(mantra::TrackedImage& image, VkFormat format, uint32_t width,
                    uint32_t height, uint32_t swizzle, const std::string& name,
                    const std::filesystem::path& path = {});
  // The traced frame's picture: <trace log>.png (g_trace_capture).
  std::filesystem::path trace_picture_;
  static bool TraceCaptureEnabled();
  // MANTRA_CAPTURE_DIR + traced frame: dumps each pass's output.
  bool CapturingPasses();
  bool CapturingDraws();
  uint32_t pass_capture_index_ = 0;
  // Set around DrawGuest in a traced frame: logs the draw's fixed-function state.
  bool trace_current_draw_ = false;
  const char* draw_fail_reason_ = nullptr;  // why the last guest draw failed (trace)
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

  // Frame pacing: where each frame's wall time went (GPU thread), for
  // stutter hunting. Reported per stats window, and per frame for hitches.
  struct FrameTiming {
    std::chrono::steady_clock::time_point last_swap{};
    // This frame.
    uint64_t fence_ns = 0, draw_ns = 0, copy_ns = 0, swap_ns = 0;
    size_t pipelines = 0;
    uint64_t modules = 0, texture_bytes = 0, buffer_bytes = 0, vertex_bytes = 0, expand_ns = 0;
    uint64_t texture_ns[5] = {};  // load, watch, hash, swap, untile (TextureCache::Stats)
    // Where draw time went: textures (their loads included), geometry,
    // constants and descriptor sets, render targets, pipelines (DrawGuest);
    // memexport draws run on the CPU, solid fills.
    uint64_t phase_ns[7] = {};
    uint64_t thread_cpu_ns = 0;  // GPU thread CPU time at the last swap
    // This stats window.
    uint64_t frames = 0, over_50 = 0, over_100 = 0;
    double max_ms = 0, total_ms = 0, fence_ms = 0, draw_ms = 0, copy_ms = 0, swap_ms = 0;
    double busy_ms = 0;  // GPU thread CPU time (the rest of "other" is waiting for the game)
  };
  FrameTiming frame_timing_;
  void EndFrameTiming();

  // GPU profiling (MANTRA_GPU_PROFILE=draw|pass): timestamps after each pass
  // (and each draw in draw mode); the interval ending at a mark is charged to
  // that mark. Read back when the submission's fence is waited on.
  enum class GpuMarkKind : uint8_t {
    kStart, kSetup, kPass, kDraw, kFill, kResolve, kPresent, kTail, kConvert, kCount
  };
  struct GpuMark {
    GpuMarkKind kind;
    uint64_t a, b;
    uint32_t n, extra;
  };
  struct GpuProfileEntry {
    uint64_t ns = 0, inclusive_ns = 0, count = 0, n = 0;
    uint32_t extra = 0;
  };
  static constexpr uint32_t kGpuQueriesPerSlot = 8192;
  struct GpuProfiler {
    int mode = 0;  // 0 off, 1 passes, 2 passes and draws
    VkQueryPool pools[kFrameSlots] = {};
    std::vector<GpuMark> marks[kFrameSlots];
    double ns_per_tick = 1.0;
    uint64_t valid_mask = ~uint64_t(0);
    PFN_vkCmdWriteTimestamp write_timestamp = nullptr;
    PFN_vkGetQueryPoolResults get_results = nullptr;
    uint32_t pass_draws = 0;
    // This stats window.
    uint64_t kind_ns[size_t(GpuMarkKind::kCount)] = {};
    uint64_t overflows = 0;
    std::map<std::tuple<uint8_t, uint64_t, uint64_t>, GpuProfileEntry> entries;
  };
  GpuProfiler gpu_profiler_;
  void InitializeGpuProfiler();
  void GpuMarkHere(GpuMarkKind kind, uint64_t a = 0, uint64_t b = 0, uint32_t n = 0,
                   uint32_t extra = 0);
  void CollectGpuProfile(uint32_t slot_index);
  void ReportGpuProfile(double frames);

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
    uint64_t float7e3_conversions = 0;
    uint64_t float7e3_conversions_skipped = 0;  // nothing written since the last one
    uint64_t depth_copies = 0;
    uint64_t depth_copies_skipped = 0;
    uint64_t unsupported_dest_format = 0;
    uint64_t color_clears = 0;
    uint64_t depth_clears = 0;
    uint64_t fills_color = 0;  // solid fills executed as clears
    uint64_t fills_depth = 0;
    uint64_t fills_skipped = 0;
  };
  DrawStats draw_stats_;
  mantra::MemExportStats memexport_stats_;
  // Distinct fill / resolve routes already logged (debug).
  std::unordered_map<uint64_t, uint64_t> logged_routes_;
  bool FirstRoute(uint64_t key) { return logged_routes_.emplace(key, 0).second; }

  // Shader capture (see LoadShader / MANTRA_SHADER_DUMP).
  void DumpShader(uint64_t hash, bool is_pixel, const uint32_t* microcode, size_t byte_count);
  // Non-exact cache matches, into <cache dir>/shader_dumps for the next build.
  void KeepForNextCacheBuild(uint64_t hash, bool is_pixel, const uint32_t* microcode,
                             size_t byte_count);
  struct ShaderStats {
    uint64_t loads = 0;
    uint64_t unique = 0;
    uint64_t dumped = 0;
    // Unique shaders by cache result.
    uint64_t cache_exact = 0;
    uint64_t cache_normalized = 0;
    uint64_t remapped = 0;  // normalized matches with renumbered interpolators
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
  // The launcher rebuilds the cache while the game runs (shaders the game
  // used that it lacked); checked every few seconds at swap.
  void ReloadShaderCacheIfChanged();
  // Fills in a shader's translation from a cache lookup; returns whether it
  // got a shader module.
  bool AttachTranslation(MantraShader& shader, const mantra::ShaderCache::Lookup& lookup,
                         const uint32_t* microcode_be, size_t byte_count);
  std::filesystem::path shader_cache_path_;
  std::filesystem::file_time_type shader_cache_time_{};
  uintmax_t shader_cache_bytes_ = 0;
  std::chrono::steady_clock::time_point shader_cache_checked_{};
  // The rebuilt cache, loaded on another thread (a load takes about a second:
  // on this one, the game froze each time the launcher added shaders).
  struct PendingShaderCache {
    std::unique_ptr<mantra::ShaderCache> cache;
    std::filesystem::file_time_type time{};
    uintmax_t bytes = 0;
  };
  std::future<PendingShaderCache> shader_cache_loading_;
  std::vector<VkShaderModule> retired_modules_;  // replaced, maybe still in flight

  const ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;

  FrameSlot frame_slots_[kFrameSlots];
  uint64_t submission_current_ = 1;  // the one being recorded
  uint64_t submission_completed_ = 0;
  bool recording_ = false;

  mantra::CopyPipelines copy_pipelines_;
  mantra::DrawPipelines draw_pipelines_;
  // Pipelines from earlier runs (MANTRA_PIPELINE_WARMUP=0 skips building them).
  mantra::PipelineStore pipeline_store_;
  void StartPipelineWarmUp();
  void RecordPipeline(const MantraShader& vs, const MantraShader* ps,
                      const mantra::PipelineKey& key);
  mantra::BufferCache buffer_cache_;
  mantra::TextureCache texture_cache_;
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
