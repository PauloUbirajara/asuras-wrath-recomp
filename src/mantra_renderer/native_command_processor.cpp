#include "native_command_processor.h"
#include "mantra_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xvideo.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::native {

namespace {

constexpr uint32_t kGuestPhysicalMemorySize = 0x20000000;

// Decodes one guest 32bpp texel (already endian-swapped) into four 16-bit
// UNORM components in fetch order (X, Y, Z, W).
inline void DecodeTexel(xenos::TextureFormat format, uint32_t v, uint16_t out[4]) {
  switch (format) {
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
      for (uint32_t i = 0; i < 3; ++i) {
        uint32_t c = (v >> (10 * i)) & 0x3FF;
        out[i] = uint16_t((c << 6) | (c >> 4));
      }
      out[3] = uint16_t((v >> 30) * 0x5555);
      break;
    default:  // k_8_8_8_8, k_8_8_8_8_AS_16_16_16_16
      for (uint32_t i = 0; i < 4; ++i) {
        out[i] = uint16_t(((v >> (8 * i)) & 0xFF) * 257);
      }
      break;
  }
}

inline uint16_t Swizzled(const uint16_t comps[4], uint32_t sel) {
  if (sel < 4) {
    return comps[sel];
  }
  return sel == xenos::XE_GPU_TEXTURE_SWIZZLE_0 ? uint16_t(0) : uint16_t(0xFFFF);
}

// Packs to VK_FORMAT_A2B10G10R10_UNORM_PACK32 (R in the low bits).
inline uint32_t PackA2B10G10R10(uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
  return uint32_t(r >> 6) | (uint32_t(g >> 6) << 10) | (uint32_t(b >> 6) << 20) |
         (uint32_t(a >> 14) << 30);
}

}  // namespace

NativeCommandProcessor::NativeCommandProcessor(GraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

NativeCommandProcessor::~NativeCommandProcessor() = default;

bool NativeCommandProcessor::SetupContext() {
  auto* provider = static_cast<ui::vulkan::VulkanProvider*>(graphics_system_->provider());
  if (!provider || !provider->vulkan_device()) {
    MANTRA_ERROR("no Vulkan device");
    return false;
  }
  vulkan_device_ = provider->vulkan_device();
  const auto& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  MANTRA_INFO("Vulkan device: {}", vulkan_device_->properties().deviceName);

  for (FrameSlot& slot : frame_slots_) {
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &slot.command_pool) != VK_SUCCESS) {
      MANTRA_ERROR("failed to create a command pool");
      DestroyFrameResources();
      return false;
    }
    VkCommandBufferAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = slot.command_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDescriptorSetsPerSlot};
    VkDescriptorPoolCreateInfo descriptor_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_info.maxSets = kDescriptorSetsPerSlot;
    descriptor_info.poolSizeCount = 1;
    descriptor_info.pPoolSizes = &pool_size;
    if (dfn.vkAllocateCommandBuffers(device, &alloc_info, &slot.command_buffer) != VK_SUCCESS ||
        dfn.vkCreateFence(device, &fence_info, nullptr, &slot.fence) != VK_SUCCESS ||
        dfn.vkCreateDescriptorPool(device, &descriptor_info, nullptr, &slot.descriptor_pool) !=
            VK_SUCCESS) {
      MANTRA_ERROR("failed to create the per-frame Vulkan objects");
      DestroyFrameResources();
      return false;
    }
  }

  if (!copy_pipelines_.Initialize(vulkan_device_) || !render_targets_.Initialize(vulkan_device_)) {
    DestroyFrameResources();
    return false;
  }
  resolved_surfaces_.Initialize(vulkan_device_);

  // Offline-translated shaders (mantra_shadergen). Optional for now: nothing
  // draws yet, lookups only feed the stats.
  const std::filesystem::path cache_path = mantra::ShaderCache::DefaultPath();
  if (!cache_path.empty() && shader_cache_.Load(cache_path)) {
    MANTRA_INFO("shader cache: {} entries from {}", shader_cache_.size(), cache_path.string());
  } else {
    MANTRA_WARN("no shader cache at {}; run mantra_shadergen --game <extracted> --out <path>",
                cache_path.string());
  }
  return true;
}

void NativeCommandProcessor::ShutdownContext() {
  ReportDrawCensus();
  DestroyFrameResources();
}

// --- Submissions -------------------------------------------------------------

void NativeCommandProcessor::AwaitSlot(FrameSlot& slot) {
  if (!slot.pending) {
    return;
  }
  const auto& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  dfn.vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
  dfn.vkResetFences(device, 1, &slot.fence);
  slot.pending = false;
  submission_completed_ = std::max(submission_completed_, slot.submission);
}

void NativeCommandProcessor::AwaitSubmission(uint64_t index) {
  if (index <= submission_completed_) {
    return;
  }
  if (index >= submission_current_ && recording_) {
    SubmitCommands();
  }
  for (FrameSlot& slot : frame_slots_) {
    if (slot.pending && slot.submission <= index) {
      AwaitSlot(slot);
    }
  }
}

VkCommandBuffer NativeCommandProcessor::BeginCommands() {
  FrameSlot& slot = CurrentSlot();
  if (recording_) {
    return slot.command_buffer;
  }
  if (!vulkan_device_ || !slot.command_buffer) {
    return VK_NULL_HANDLE;
  }
  const auto& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  AwaitSlot(slot);
  dfn.vkResetCommandPool(device, slot.command_pool, 0);
  dfn.vkResetDescriptorPool(device, slot.descriptor_pool, 0);
  slot.descriptor_sets_used = 0;
  resolved_surfaces_.Collect(submission_completed_);

  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (dfn.vkBeginCommandBuffer(slot.command_buffer, &begin_info) != VK_SUCCESS) {
    MANTRA_ERROR("vkBeginCommandBuffer failed");
    return VK_NULL_HANDLE;
  }
  recording_ = true;
  return slot.command_buffer;
}

bool NativeCommandProcessor::SubmitCommands() {
  if (!recording_) {
    return true;
  }
  recording_ = false;
  FrameSlot& slot = CurrentSlot();
  const auto& dfn = vulkan_device_->functions();
  if (dfn.vkEndCommandBuffer(slot.command_buffer) != VK_SUCCESS) {
    MANTRA_ERROR("vkEndCommandBuffer failed");
    return false;
  }
  VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &slot.command_buffer;
  VkResult result;
  {
    const auto queue_acquisition =
        vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
    result = dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, slot.fence);
  }
  if (result != VK_SUCCESS) {
    MANTRA_ERROR("vkQueueSubmit failed ({})", int32_t(result));
    return false;
  }
  slot.submission = submission_current_++;
  slot.pending = true;
  return true;
}

VkDescriptorPool NativeCommandProcessor::ReserveDescriptorSet() {
  if (recording_ && CurrentSlot().descriptor_sets_used >= kDescriptorSetsPerSlot) {
    SubmitCommands();
  }
  if (!BeginCommands()) {
    return VK_NULL_HANDLE;
  }
  FrameSlot& slot = CurrentSlot();
  ++slot.descriptor_sets_used;
  return slot.descriptor_pool;
}

void NativeCommandProcessor::DestroyFrameResources() {
  if (!vulkan_device_) {
    return;
  }
  SubmitCommands();
  for (FrameSlot& slot : frame_slots_) {
    AwaitSlot(slot);
  }
  DestroySwapResources();
  resolved_surfaces_.Shutdown();
  render_targets_.Shutdown();
  copy_pipelines_.Shutdown();
  const auto& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  for (FrameSlot& slot : frame_slots_) {
    if (slot.descriptor_pool) dfn.vkDestroyDescriptorPool(device, slot.descriptor_pool, nullptr);
    if (slot.fence) dfn.vkDestroyFence(device, slot.fence, nullptr);
    // Frees the command buffer too.
    if (slot.command_pool) dfn.vkDestroyCommandPool(device, slot.command_pool, nullptr);
    slot = FrameSlot{};
  }
}

void NativeCommandProcessor::DestroySwapResources() {
  if (!vulkan_device_) {
    return;
  }
  AwaitSubmission(staging_submission_);
  const auto& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (staging_mapping_) {
    dfn.vkUnmapMemory(device, staging_memory_);
    staging_mapping_ = nullptr;
  }
  if (staging_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, staging_buffer_, nullptr);
    staging_buffer_ = VK_NULL_HANDLE;
  }
  if (staging_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, staging_memory_, nullptr);
    staging_memory_ = VK_NULL_HANDLE;
  }
  staging_buffer_size_ = 0;
}

bool NativeCommandProcessor::EnsureStagingBuffer(VkDeviceSize size) {
  if (staging_buffer_ != VK_NULL_HANDLE && staging_buffer_size_ >= size) {
    return true;
  }
  DestroySwapResources();
  const auto& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, staging_buffer_, staging_memory_,
          &staging_memory_type_, &staging_memory_size_)) {
    MANTRA_ERROR("failed to create a {}-byte swap staging buffer", size);
    return false;
  }
  if (dfn.vkMapMemory(device, staging_memory_, 0, VK_WHOLE_SIZE, 0, &staging_mapping_) !=
      VK_SUCCESS) {
    MANTRA_ERROR("failed to map the swap staging buffer");
    dfn.vkDestroyBuffer(device, staging_buffer_, nullptr);
    dfn.vkFreeMemory(device, staging_memory_, nullptr);
    staging_buffer_ = VK_NULL_HANDLE;
    staging_memory_ = VK_NULL_HANDLE;
    staging_mapping_ = nullptr;
    return false;
  }
  staging_buffer_size_ = size;
  return true;
}

// --- Draws and resolves ------------------------------------------------------

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t, IndexBufferInfo*,
                                       bool) {
  const xenos::EdramMode edram_mode = register_file_->Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Not a draw: the RB copies EDRAM to memory, the shaders never run.
    ++draw_stats_.copy_draws;
    return IssueCopy();
  }
  ++draw_stats_.draws;
  TrySolidFill(prim_type, edram_mode);
  {
    Shader* vs = active_vertex_shader();
    Shader* ps = active_pixel_shader();
    ++draw_census_[{vs ? vs->ucode_data_hash() : 0, ps ? ps->ucode_data_hash() : 0,
                    uint32_t(edram_mode) | (uint32_t(prim_type) << 8)}];
  }
  return true;
}

bool NativeCommandProcessor::IssueCopy() {
  if (!Resolve()) {
    ++draw_stats_.resolves_failed;
  }
  // A failed resolve is dropped; it must not abort the command stream.
  return true;
}

bool NativeCommandProcessor::Resolve() {
  const RegisterFile& regs = *register_file_;
  draw_util::ResolveInfo info;
  if (!draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info)) {
    return false;
  }
  if (!info.coordinate_info.width_div_8 || !info.height_div_8) {
    return true;
  }

  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const uint32_t pitch_px = surface_info.surface_pitch;
  const xenos::MsaaSamples msaa = surface_info.msaa_samples;
  const uint32_t surface_pitch_tiles = xenos::GetSurfacePitchTiles(pitch_px, msaa, false);
  if (!surface_pitch_tiles) {
    return false;
  }

  // Undo GetResolveInfo's base adjustment to get the rectangle in surface
  // pixels: base_tiles = original + (tile offset << is_64bpp).
  const bool copying_depth = info.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& edram =
      copying_depth || !info.color_edram_info.packed ? info.depth_edram_info
                                                     : info.color_edram_info;
  const uint32_t original_base = copying_depth || !info.color_edram_info.packed
                                     ? info.depth_original_base
                                     : info.color_original_base;
  const uint32_t offset_tiles = ((edram.base_tiles - original_base) & (xenos::kEdramTileCount - 1)) >>
                                edram.format_is_64bpp;
  const uint32_t samples_log2_x = msaa >= xenos::MsaaSamples::k4X ? 1 : 0;
  const uint32_t samples_log2_y = msaa >= xenos::MsaaSamples::k2X ? 1 : 0;
  mantra::CopyRect rect;
  rect.x = int32_t((((offset_tiles % surface_pitch_tiles) * xenos::kEdramTileWidthSamples) >>
                    samples_log2_x) +
                   (info.coordinate_info.edram_offset_x_div_8 << 3));
  rect.y = int32_t((((offset_tiles / surface_pitch_tiles) * xenos::kEdramTileHeightSamples) >>
                    samples_log2_y) +
                   (info.coordinate_info.edram_offset_y_div_8 << 3));
  rect.width = info.coordinate_info.width_div_8 << 3;
  rect.height = info.height_div_8 << 3;
  const VkRect2D clear_rect = {{rect.x, rect.y}, {rect.width, rect.height}};

  VkCommandBuffer cmd = BeginCommands();
  if (!cmd) {
    return false;
  }
  const uint64_t frame = submission_current_;

  const auto color_info = regs.Get<reg::RB_COLOR_INFO>(
      reg::RB_COLOR_INFO::rt_register_indices[info.rb_copy_control.copy_src_select & 3]);

  // Copy.
  if (copying_depth) {
    ++draw_stats_.depth_copies_skipped;
    MANTRA_WARN_ONCE("depth resolves are not implemented yet, skipping");
  } else if (info.copy_dest_extent_length) {
    const xenos::ColorFormat dest_format = info.copy_dest_info.copy_dest_format;
    const VkFormat dest_host_format = mantra::ResolvedSurfaces::HostFormat(dest_format);
    mantra::RenderTarget* rt = render_targets_.GetColor(
        cmd, info.color_original_base, pitch_px, msaa, color_info.color_format, frame);
    if (dest_host_format == VK_FORMAT_UNDEFINED) {
      ++draw_stats_.unsupported_dest_format;
      MANTRA_WARN_ONCE("resolve to unsupported format {}, skipping", uint32_t(dest_format));
    } else if (rt) {
      const auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
      const uint32_t dest_address =
          regs[XE_GPU_REG_RB_COPY_DEST_BASE] & (kGuestPhysicalMemorySize - 1);
      // Keep the source inside the render target.
      rect.width = std::min<uint32_t>(rect.width, rt->image.width - std::min<uint32_t>(
                                                                         rect.x, rt->image.width));
      rect.height = std::min<uint32_t>(
          rect.height, rt->image.height - std::min<uint32_t>(rect.y, rt->image.height));
      mantra::ResolvedSurface* surface = resolved_surfaces_.Get(
          dest_address, std::max<uint32_t>(dest_pitch.copy_dest_pitch, rect.x + rect.width),
          std::max<uint32_t>(dest_pitch.copy_dest_height, rect.y + rect.height), dest_format,
          frame);
      VkDescriptorPool descriptor_pool = ReserveDescriptorSet();
      cmd = BeginCommands();  // may have been submitted to free descriptors
      if (surface && descriptor_pool && cmd && rect.width && rect.height) {
        mantra::CopyPushConstants constants;
        const uint32_t sample_count = 1u << uint32_t(msaa);
        static constexpr uint32_t kSampleMasks[] = {0x1, 0x2, 0x4, 0x8, 0x3, 0xC, 0xF, 0xF};
        constants.sample_mask =
            kSampleMasks[uint32_t(info.copy_dest_coordinate_info.copy_sample_select) & 7] &
            ((1u << sample_count) - 1);
        if (!constants.sample_mask) {
          constants.sample_mask = 1;
        }
        constants.scale = std::ldexp(1.0f, info.copy_dest_info.copy_dest_exp_bias);
        if (info.copy_dest_info.copy_dest_swap) {
          constants.swizzle[0] = 2;
          constants.swizzle[2] = 0;
        }
        mantra::TransitionImage(vulkan_device_, cmd, rt->image,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        mantra::TransitionImage(vulkan_device_, cmd, surface->image,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                mantra::kColorAttachmentStage, mantra::kColorAttachmentAccess);
        const VkImageView src_view = render_targets_.GetView(
            *rt, mantra::RenderTargetPool::HostColorFormat(color_info.color_format));
        if (src_view &&
            copy_pipelines_.Record(cmd, descriptor_pool, src_view, msaa != xenos::MsaaSamples::k1X,
                                   surface->view, dest_host_format, surface->image.width,
                                   surface->image.height, rect, constants)) {
          ++draw_stats_.color_copies;
          ++surface->write_count;
          surface->last_write_frame = frame;
        }
        if (FirstRoute((uint64_t(info.color_original_base) << 40) ^ (uint64_t(pitch_px) << 26) ^
                       (uint64_t(color_info.color_format) << 22) ^ dest_address ^
                       (uint64_t(dest_format) << 52) ^ (uint64_t(1) << 63)))
          MANTRA_INFO(
            "resolve: rt base {} pitch {} fmt {} rect {},{} {}x{} -> 0x{:08X} fmt {} pitch {}{}",
            info.color_original_base, pitch_px, uint32_t(color_info.color_format), rect.x, rect.y,
            rect.width, rect.height, dest_address, uint32_t(dest_format),
            uint32_t(dest_pitch.copy_dest_pitch), info.IsClearingColor() ? " +clear" : "");
      }
    }
  }

  // Clears (after the copy, of the same rectangle).
  cmd = BeginCommands();
  if (!cmd) {
    return false;
  }
  if (info.IsClearingColor()) {
    if (mantra::RenderTarget* rt = render_targets_.GetColor(
            cmd, info.color_original_base, pitch_px, msaa, color_info.color_format, frame)) {
      render_targets_.ClearColor(cmd, *rt, color_info.color_format, clear_rect,
                                 uint64_t(info.rb_color_clear) |
                                     (uint64_t(info.rb_color_clear_lo) << 32));
      ++draw_stats_.color_clears;
    }
  }
  if (info.IsClearingDepth()) {
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    if (mantra::RenderTarget* rt =
            render_targets_.GetDepth(cmd, info.depth_original_base, pitch_px, msaa, frame)) {
      render_targets_.ClearDepth(cmd, *rt, depth_info.depth_format, clear_rect,
                                 info.rb_depth_clear);
      ++draw_stats_.depth_clears;
    }
  }
  return true;
}

// --- Swap --------------------------------------------------------------------

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  ++swap_stats_.swaps;
  ReportSwapStats();
  if (!graphics_system_ || !vulkan_device_) {
    MANTRA_WARN_ONCE("IssueSwap before the Vulkan device was set up, skipping present");
    ++swap_stats_.skipped;
    return;
  }
  // Whatever happens below, this frame's resolves get submitted.
  struct SubmitAtExit {
    NativeCommandProcessor* self;
    ~SubmitAtExit() { self->SubmitCommands(); }
  } submit_at_exit{this};

  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    MANTRA_WARN_ONCE("IssueSwap with no presenter (headless?), skipping present");
    ++swap_stats_.skipped;
    return;
  }

  // As on the Xenia-derived backends, the frontbuffer is described by texture
  // fetch constant 0 at swap time.
  const xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(0);
  const xenos::TextureFormat format = fetch.format;
  MANTRA_NOISY_DEBUG(
      "swap #{}: packet ptr=0x{:08X} {}x{}, fetch0 base=0x{:08X} {}x{} fmt={} pitch={} "
      "tiled={} endian={} swizzle=0x{:03X}",
      swap_stats_.swaps, frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
      uint32_t(fetch.base_address) << 12, fetch.size_2d.width + 1, fetch.size_2d.height + 1,
      uint32_t(format), uint32_t(fetch.pitch) << 5, uint32_t(fetch.tiled),
      uint32_t(fetch.endianness), uint32_t(fetch.swizzle));
  switch (format) {
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16:
    case xenos::TextureFormat::k_2_10_10_10:
    case xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16:
      break;
    default:
      MANTRA_WARN_ONCE("unsupported frontbuffer format {}, skipping present",
                       uint32_t(format));
      ++swap_stats_.skipped;
      return;
  }
  uint32_t base_address = fetch.base_address << 12;
  if (!base_address) {
    base_address = frontbuffer_ptr & (kGuestPhysicalMemorySize - 1);
  }
  if (!base_address) {
    MANTRA_WARN_ONCE("swap with no frontbuffer address, skipping present");
    ++swap_stats_.skipped;
    return;
  }

  const uint32_t texture_width = fetch.size_2d.width + 1;
  const uint32_t texture_height = fetch.size_2d.height + 1;
  const uint32_t width =
      frontbuffer_width ? std::min(frontbuffer_width, texture_width) : texture_width;
  const uint32_t height =
      frontbuffer_height ? std::min(frontbuffer_height, texture_height) : texture_height;
  // Pitch is in 32-pixel units; fall back to the width when unset.
  const uint32_t pitch = fetch.pitch ? (fetch.pitch << 5) : ((texture_width + 31) & ~31u);
  const bool tiled = fetch.tiled != 0;
  const bool is_8bpc = format == xenos::TextureFormat::k_8_8_8_8 ||
                       format == xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16;

  // Prefer the GPU copy of the frontbuffer when the guest resolved into it.
  mantra::ResolvedSurface* surface = resolved_surfaces_.Find(base_address);
  const bool use_gpu = surface && surface->write_count;

  // Log the frontbuffer description on the first swap and whenever it changes.
  const FrontbufferDesc desc{base_address, width, height, pitch, uint32_t(format), tiled,
                             uint32_t(fetch.endianness), uint32_t(fetch.swizzle)};
  if (!(desc == last_frontbuffer_) || use_gpu != last_present_gpu_) {
    MANTRA_INFO(
        "frontbuffer {}: 0x{:08X} {}x{} pitch {} format {} {} endian {} swizzle 0x{:03X}, "
        "presenting from {} (swap #{})",
        last_frontbuffer_.width ? "changed" : "first seen", desc.address, desc.width,
        desc.height, desc.pitch, desc.format, desc.tiled ? "tiled" : "linear", desc.endian,
        desc.swizzle,
        use_gpu ? fmt::format("resolved surface (format {}, {}x{})", uint32_t(surface->format),
                              surface->image.width, surface->image.height)
                : std::string("guest memory"),
        swap_stats_.swaps);
    last_frontbuffer_ = desc;
    last_present_gpu_ = use_gpu;
  }

  bool presented;
  if (use_gpu) {
    presented = PresentResolvedSurface(*surface, width, height, fetch.swizzle, is_8bpc);
    swap_stats_.presented_gpu += presented;
    static const char* capture_dir = std::getenv("MANTRA_CAPTURE_DIR");
    if (presented && capture_dir && *capture_dir && swap_stats_.swaps % 150 == 0) {
      CaptureSurface(*surface, width, height, fetch.swizzle,
                     fmt::format("frame_{:06}", swap_stats_.swaps));
      resolved_surfaces_.ForEach([&](mantra::ResolvedSurface& other) {
        if (&other != surface && other.write_count) {
          CaptureSurface(other, other.pitch, other.height, 0x688 /* RGBA */,
                         fmt::format("frame_{:06}_surf_{:08X}_fmt{}", swap_stats_.swaps,
                                     other.address, uint32_t(other.format)));
        }
      });
    }
  } else {
    // Reject frontbuffers that would read past the end of physical memory.
    const uint32_t last_offset =
        tiled ? uint32_t(texture_util::GetTiledOffset2D(int32_t(width - 1), int32_t(height - 1),
                                                        pitch, 2))
              : ((height - 1) * pitch + (width - 1)) * 4;
    if (uint64_t(base_address) + last_offset + 4 > kGuestPhysicalMemorySize) {
      MANTRA_WARN_ONCE("frontbuffer 0x{:08X} {}x{} pitch {} out of range, skipping present",
                       base_address, width, height, pitch);
      ++swap_stats_.skipped;
      return;
    }
    presented = PresentFromGuestMemory(memory_->TranslatePhysical(base_address), width, height,
                                       pitch, tiled, format, fetch.endianness, fetch.swizzle,
                                       is_8bpc);
    swap_stats_.presented_cpu += presented;
  }
  if (presented) {
    ++swap_stats_.presented;
  } else {
    ++swap_stats_.refresh_failed;
    MANTRA_WARN_ONCE("presenter refresh failed for a {}x{} guest output", width, height);
  }
}

namespace {

// Moves the presenter's guest output image into / out of the color attachment
// state. Its prior state is defined by the presenter (VulkanPresenter docs).
mantra::TrackedImage GuestOutputImage(
    const ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext& context, uint32_t width,
    uint32_t height) {
  mantra::TrackedImage image;
  image.image = context.image();
  image.format = ui::vulkan::VulkanPresenter::kGuestOutputFormat;
  image.width = width;
  image.height = height;
  if (context.image_ever_written_previously()) {
    image.layout = ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
    image.stage = ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask;
    image.access = ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
  }
  return image;
}

}  // namespace

bool NativeCommandProcessor::PresentResolvedSurface(mantra::ResolvedSurface& surface,
                                                    uint32_t width, uint32_t height,
                                                    uint32_t swizzle, bool is_8bpc) {
  ui::Presenter* presenter = graphics_system_->presenter();
  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  const uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  const uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  return presenter->RefreshGuestOutput(
      width, height, display_width, display_height,
      [&](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vk_context =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        context.SetIs8bpc(is_8bpc);
        VkDescriptorPool descriptor_pool = ReserveDescriptorSet();
        VkCommandBuffer cmd = BeginCommands();
        if (!descriptor_pool || !cmd) {
          return false;
        }
        mantra::TransitionImage(vulkan_device_, cmd, surface.image,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        mantra::TrackedImage output = GuestOutputImage(vk_context, width, height);
        // Fully overwritten below if the surface covers it; otherwise the
        // uncovered part is undefined anyway.
        mantra::TransitionImage(vulkan_device_, cmd, output,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                mantra::kColorAttachmentStage, mantra::kColorAttachmentAccess,
                                /*discard=*/true);
        mantra::CopyPushConstants constants;
        for (uint32_t i = 0; i < 4; ++i) {
          constants.swizzle[i] = (swizzle >> (3 * i)) & 7;
        }
        mantra::CopyRect rect;
        rect.width = std::min(width, surface.image.width);
        rect.height = std::min(height, surface.image.height);
        const bool recorded = copy_pipelines_.Record(
            cmd, descriptor_pool, surface.view, false, vk_context.image_view(),
            ui::vulkan::VulkanPresenter::kGuestOutputFormat, width, height, rect, constants);
        mantra::TransitionImage(vulkan_device_, cmd, output,
                                ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout,
                                ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask);
        // Submit now: the presenter may paint the image as soon as this
        // returns, and its submission must come after ours.
        return SubmitCommands() && recorded;
      });
}

void NativeCommandProcessor::CaptureSurface(mantra::ResolvedSurface& surface, uint32_t width,
                                            uint32_t height, uint32_t swizzle,
                                            const std::string& name) {
  const VkFormat format = surface.image.format;
  if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
    return;
  }
  width = std::min(width, surface.image.width);
  height = std::min(height, surface.image.height);
  const VkDeviceSize size = VkDeviceSize(width) * height * 4;
  const auto& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint32_t memory_type = 0;
  VkDeviceSize memory_size = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device_, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, buffer, memory, &memory_type,
          &memory_size)) {
    return;
  }
  VkCommandBuffer cmd = BeginCommands();
  if (cmd) {
    mantra::TransitionImage(vulkan_device_, cmd, surface.image,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    dfn.vkCmdCopyImageToBuffer(cmd, surface.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buffer, 1, &region);
    const uint64_t submission = submission_current_;
    SubmitCommands();
    AwaitSubmission(submission);
    void* mapping = nullptr;
    if (dfn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapping) == VK_SUCCESS) {
      VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = memory;
      range.size = VK_WHOLE_SIZE;
      dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
      const uint32_t* texels = static_cast<const uint32_t*>(mapping);
      const std::filesystem::path path = std::filesystem::path(std::getenv("MANTRA_CAPTURE_DIR")) /
                                         (name + ".ppm");
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        std::vector<uint8_t> row(size_t(width) * 3);
        for (uint32_t y = 0; y < height; ++y) {
          for (uint32_t x = 0; x < width; ++x) {
            const uint32_t v = texels[size_t(y) * width + x];
            uint16_t comps[4];
            if (format == VK_FORMAT_R8G8B8A8_UNORM) {
              for (uint32_t i = 0; i < 4; ++i) comps[i] = uint16_t(((v >> (8 * i)) & 0xFF) * 257);
            } else {
              for (uint32_t i = 0; i < 3; ++i) {
                const uint32_t c = (v >> (10 * i)) & 0x3FF;
                comps[i] = uint16_t((c << 6) | (c >> 4));
              }
              comps[3] = uint16_t((v >> 30) * 0x5555);
            }
            for (uint32_t i = 0; i < 3; ++i) {
              row[x * 3 + i] = uint8_t(Swizzled(comps, (swizzle >> (3 * i)) & 7) >> 8);
            }
          }
          std::fwrite(row.data(), 1, row.size(), file);
        }
        std::fclose(file);
        MANTRA_INFO("captured {}", path.string());
      }
      dfn.vkUnmapMemory(device, memory);
    }
  }
  dfn.vkDestroyBuffer(device, buffer, nullptr);
  dfn.vkFreeMemory(device, memory, nullptr);
}

bool NativeCommandProcessor::PresentFromGuestMemory(const uint8_t* src, uint32_t width,
                                                    uint32_t height, uint32_t pitch, bool tiled,
                                                    xenos::TextureFormat format,
                                                    xenos::Endian endian, uint32_t swizzle,
                                                    bool is_8bpc) {
  // Make sure the previous upload no longer reads the staging buffer.
  AwaitSubmission(staging_submission_);
  const VkDeviceSize upload_size = VkDeviceSize(width) * height * 4;
  if (!EnsureStagingBuffer(upload_size)) {
    return false;
  }

  // Untile, endian-swap, swizzle and convert to A2B10G10R10 on the CPU.
  uint32_t* dst = static_cast<uint32_t*>(staging_mapping_);
  const uint32_t sel_r = swizzle & 7, sel_g = (swizzle >> 3) & 7, sel_b = (swizzle >> 6) & 7,
                 sel_a = (swizzle >> 9) & 7;
  // Count non-black pixels so the logs show whether the guest frontbuffer
  // actually has content.
  uint64_t lit_pixels = 0;
  for (uint32_t y = 0; y < height; ++y) {
    uint32_t* dst_row = dst + size_t(y) * width;
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t offset =
          tiled ? uint32_t(texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, 2))
                : (y * pitch + x) * 4;
      uint32_t raw;
      std::memcpy(&raw, src + offset, sizeof(raw));
      uint16_t comps[4];
      DecodeTexel(format, xenos::GpuSwap(raw, endian), comps);
      const uint32_t packed = PackA2B10G10R10(Swizzled(comps, sel_r), Swizzled(comps, sel_g),
                                              Swizzled(comps, sel_b), Swizzled(comps, sel_a));
      dst_row[x] = packed;
      lit_pixels += (packed & 0x3FFFFFFF) != 0;
    }
  }
  swap_stats_.last_lit_permille = uint32_t(lit_pixels * 1000 / (uint64_t(width) * height));
  ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, staging_memory_, staging_memory_type_,
                                           0, staging_memory_size_, upload_size);

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  const uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  const uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  ui::Presenter* presenter = graphics_system_->presenter();
  return presenter->RefreshGuestOutput(
      width, height, display_width, display_height,
      [&](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vk_context =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        const auto& dfn = vulkan_device_->functions();
        context.SetIs8bpc(is_8bpc);
        VkCommandBuffer cmd = BeginCommands();
        if (!cmd) {
          return false;
        }
        // The whole image is overwritten, so the old contents can be discarded.
        mantra::TrackedImage output = GuestOutputImage(vk_context, width, height);
        mantra::TransitionImage(vulkan_device_, cmd, output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                /*discard=*/true);
        VkBufferImageCopy region = {};
        region.bufferRowLength = width;
        region.bufferImageHeight = height;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {width, height, 1};
        dfn.vkCmdCopyBufferToImage(cmd, staging_buffer_, vk_context.image(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        mantra::TransitionImage(vulkan_device_, cmd, output,
                                ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout,
                                ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask);
        staging_submission_ = submission_current_;
        return SubmitCommands();
      });
}

void NativeCommandProcessor::ReportSwapStats() {
  using clock = std::chrono::steady_clock;
  constexpr auto kReportInterval = std::chrono::seconds(5);
  const auto now = clock::now();
  if (swap_stats_.last_report_time.time_since_epoch().count() == 0) {
    swap_stats_.last_report_time = now;
    swap_stats_.swaps_at_last_report = swap_stats_.swaps;
    return;
  }
  const auto elapsed = now - swap_stats_.last_report_time;
  if (elapsed < kReportInterval) {
    return;
  }
  const double seconds = std::chrono::duration<double>(elapsed).count();
  const double swaps_per_second =
      double(swap_stats_.swaps - swap_stats_.swaps_at_last_report) / seconds;
  MANTRA_INFO(
      "swaps: {} total ({:.1f}/s), presented {}, skipped {}, refresh failed {}; last frame "
      "{:.1f}% non-black",
      swap_stats_.swaps, swaps_per_second, swap_stats_.presented, swap_stats_.skipped,
      swap_stats_.refresh_failed, swap_stats_.last_lit_permille / 10.0);
  MANTRA_INFO("  presented {} from resolves, {} from guest memory; draws {}, resolves {} "
              "(failed {}, color copies {}, depth copies skipped {}, unsupported dest {}), "
              "clears color {} depth {}; solid fills color {} depth {} skipped {}; {} render targets, "
              "{} resolved surfaces",
              swap_stats_.presented_gpu, swap_stats_.presented_cpu, draw_stats_.draws,
              draw_stats_.copy_draws, draw_stats_.resolves_failed, draw_stats_.color_copies,
              draw_stats_.depth_copies_skipped, draw_stats_.unsupported_dest_format,
              draw_stats_.color_clears, draw_stats_.depth_clears, draw_stats_.fills_color,
              draw_stats_.fills_depth, draw_stats_.fills_skipped, render_targets_.size(),
              resolved_surfaces_.size());
  MANTRA_INFO("shaders: {} loads, {} unique, {} dumped; cache {} exact, {} normalized, "
              "{} failed, {} miss",
              shader_stats_.loads, shader_stats_.unique, shader_stats_.dumped,
              shader_stats_.cache_exact, shader_stats_.cache_normalized,
              shader_stats_.cache_failed, shader_stats_.cache_miss);
  swap_stats_.last_report_time = now;
  swap_stats_.swaps_at_last_report = swap_stats_.swaps;
  // The draw census every 30 seconds.
  if (++swap_stats_.reports % 6 == 0) {
    ReportDrawCensus();
  }
}

Shader* NativeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  // Key = XXH3 over the raw big-endian microcode exactly as uploaded.
  const size_t byte_count = size_t(dword_count) * sizeof(uint32_t);
  const uint64_t hash = XXH3_64bits(host_address, byte_count);
  const bool is_pixel = shader_type == xenos::ShaderType::kPixel;
  // Note: inline (IM_LOAD_IMMEDIATE) and IM_LOAD programs can't be told apart
  // here: the ring buffer sits in physical memory, so both pointers look alike.
  (void)guest_address;
  ++shader_stats_.loads;
  auto& shaders = shaders_[is_pixel ? 1 : 0];
  if (auto it = shaders.find(hash); it != shaders.end()) {
    return it->second.get();
  }
  {
    ++shader_stats_.unique;
    MANTRA_DEBUG("new {} shader {:016X}: {} dwords", is_pixel ? "pixel" : "vertex", hash,
                 dword_count);
    DumpShader(hash, is_pixel, host_address, byte_count);
    const auto lookup = shader_cache_.Find(host_address, byte_count, is_pixel);
    switch (lookup.match) {
      case mantra::ShaderCache::Match::kExact: ++shader_stats_.cache_exact; break;
      case mantra::ShaderCache::Match::kNormalized: ++shader_stats_.cache_normalized; break;
      case mantra::ShaderCache::Match::kFailed: ++shader_stats_.cache_failed; break;
      case mantra::ShaderCache::Match::kNone:
        ++shader_stats_.cache_miss;
        MANTRA_DEBUG("shader {:016X} not in the shader cache", hash);
        break;
    }
  }
  auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  shader->AnalyzeUcode(ucode_disasm_buffer_);
  Shader* result = shader.get();
  shaders.emplace(hash, std::move(shader));
  return result;
}

void NativeCommandProcessor::ReportDrawCensus() {
  std::vector<std::pair<uint64_t, std::tuple<uint64_t, uint64_t, uint32_t>>> entries;
  for (const auto& [key, count] : draw_census_) entries.emplace_back(count, key);
  std::sort(entries.begin(), entries.end(), std::greater<>());
  auto dwords = [&](uint32_t type, uint64_t hash) -> size_t {
    auto it = shaders_[type].find(hash);
    return it != shaders_[type].end() ? it->second->ucode_dword_count() : 0;
  };
  MANTRA_INFO("draw census: {} distinct (vs, ps, edram mode, prim)", entries.size());
  for (size_t i = 0; i < std::min<size_t>(entries.size(), 16); ++i) {
    const auto& [count, key] = entries[i];
    const auto& [vs, ps, mode] = key;
    MANTRA_INFO("  {:7} x vs {:016X} ({} dw) ps {:016X} ({} dw) edram {} prim {}", count, vs,
                dwords(0, vs), ps, dwords(1, ps), mode & 0xFF, mode >> 8);
  }
}

void NativeCommandProcessor::DumpShader(uint64_t hash, bool is_pixel, const uint32_t* microcode,
                                        size_t byte_count) {
  // MANTRA_SHADER_DUMP=<dir> writes each unique program once, as raw
  // big-endian microcode: {vs|ps}_<xxh3>_<dwords>.mc
  static const char* dump_dir = std::getenv("MANTRA_SHADER_DUMP");
  if (!dump_dir || !*dump_dir) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dump_dir, ec);
  const std::string name =
      fmt::format("{}_{:016X}_{}.mc", is_pixel ? "ps" : "vs", hash, byte_count / 4);
  const std::filesystem::path path = std::filesystem::path(dump_dir) / name;
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    MANTRA_WARN_ONCE("cannot write shader dumps to {}", dump_dir);
    return;
  }
  std::fwrite(microcode, 1, byte_count, file);
  std::fclose(file);
  ++shader_stats_.dumped;
}

}  // namespace rex::graphics::native
