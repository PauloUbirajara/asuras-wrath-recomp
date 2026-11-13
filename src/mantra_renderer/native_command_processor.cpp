#include "native_command_processor.h"
#include "mantra_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
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

  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool_info.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &swap_command_pool_) != VK_SUCCESS) {
    MANTRA_ERROR("failed to create the swap command pool");
    return false;
  }

  VkCommandBufferAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc_info.commandPool = swap_command_pool_;
  alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(device, &alloc_info, &swap_command_buffer_) != VK_SUCCESS) {
    MANTRA_ERROR("failed to allocate the swap command buffer");
    DestroySwapResources();
    return false;
  }

  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (dfn.vkCreateFence(device, &fence_info, nullptr, &swap_fence_) != VK_SUCCESS) {
    MANTRA_ERROR("failed to create the swap fence");
    DestroySwapResources();
    return false;
  }

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
  DestroySwapResources();
}

void NativeCommandProcessor::WaitForSwapUpload() {
  if (!swap_fence_pending_) {
    return;
  }
  const auto& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  dfn.vkWaitForFences(device, 1, &swap_fence_, VK_TRUE, UINT64_MAX);
  dfn.vkResetFences(device, 1, &swap_fence_);
  dfn.vkResetCommandPool(device, swap_command_pool_, 0);
  swap_fence_pending_ = false;
}

void NativeCommandProcessor::DestroySwapResources() {
  if (!vulkan_device_) {
    return;
  }
  WaitForSwapUpload();
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
  if (swap_fence_ != VK_NULL_HANDLE) {
    dfn.vkDestroyFence(device, swap_fence_, nullptr);
    swap_fence_ = VK_NULL_HANDLE;
  }
  if (swap_command_pool_ != VK_NULL_HANDLE) {
    // Frees swap_command_buffer_ too.
    dfn.vkDestroyCommandPool(device, swap_command_pool_, nullptr);
    swap_command_pool_ = VK_NULL_HANDLE;
    swap_command_buffer_ = VK_NULL_HANDLE;
  }
}

bool NativeCommandProcessor::EnsureStagingBuffer(VkDeviceSize size) {
  if (staging_buffer_ != VK_NULL_HANDLE && staging_buffer_size_ >= size) {
    return true;
  }
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

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  ++swap_stats_.swaps;
  ReportSwapStats();
  if (!graphics_system_ || !vulkan_device_) {
    MANTRA_WARN_ONCE("IssueSwap before the Vulkan device was set up, skipping present");
    ++swap_stats_.skipped;
    return;
  }
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

  // Log the frontbuffer description on the first swap and whenever it changes.
  const FrontbufferDesc desc{base_address, width, height, pitch, uint32_t(format), tiled,
                             uint32_t(fetch.endianness), uint32_t(fetch.swizzle)};
  if (!(desc == last_frontbuffer_)) {
    MANTRA_INFO(
        "frontbuffer {}: 0x{:08X} {}x{} pitch {} format {} {} endian {} swizzle 0x{:03X} "
        "(swap #{})",
        last_frontbuffer_.width ? "changed" : "first seen", desc.address, desc.width,
        desc.height, desc.pitch, desc.format, desc.tiled ? "tiled" : "linear", desc.endian,
        desc.swizzle, swap_stats_.swaps);
    last_frontbuffer_ = desc;
  }

  // Make sure the previous upload no longer reads the staging buffer.
  WaitForSwapUpload();
  const VkDeviceSize upload_size = VkDeviceSize(width) * height * 4;
  if (!EnsureStagingBuffer(upload_size)) {
    ++swap_stats_.skipped;
    return;
  }

  // Untile, endian-swap, swizzle and convert to A2B10G10R10 on the CPU.
  const uint8_t* src = memory_->TranslatePhysical(base_address);
  uint32_t* dst = static_cast<uint32_t*>(staging_mapping_);
  const xenos::Endian endian = fetch.endianness;
  const uint32_t swizzle = fetch.swizzle;
  const uint32_t sel_r = swizzle & 7, sel_g = (swizzle >> 3) & 7, sel_b = (swizzle >> 6) & 7,
                 sel_a = (swizzle >> 9) & 7;
  // Count non-black pixels so the logs show whether the guest frontbuffer
  // actually has content (it stays black while IssueCopy is a stub).
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

  const bool is_8bpc = format == xenos::TextureFormat::k_8_8_8_8 ||
                       format == xenos::TextureFormat::k_8_8_8_8_AS_16_16_16_16;

  const bool presented = presenter->RefreshGuestOutput(
      width, height, display_width, display_height,
      [this, width, height, is_8bpc](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vk_context =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        const auto& dfn = vulkan_device_->functions();
        context.SetIs8bpc(is_8bpc);

        VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dfn.vkBeginCommandBuffer(swap_command_buffer_, &begin_info) != VK_SUCCESS) {
          return false;
        }

        // The whole image is overwritten, so the old contents can be discarded;
        // only wait for the presenter's previous reads.
        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vk_context.image();
        barrier.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
        dfn.vkCmdPipelineBarrier(
            swap_command_buffer_,
            vk_context.image_ever_written_previously()
                ? VkPipelineStageFlags(ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask)
                : VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region = {};
        region.bufferOffset = 0;
        region.bufferRowLength = width;
        region.bufferImageHeight = height;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {width, height, 1};
        dfn.vkCmdCopyBufferToImage(swap_command_buffer_, staging_buffer_, vk_context.image(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
        dfn.vkCmdPipelineBarrier(swap_command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask, 0, 0,
                                 nullptr, 0, nullptr, 1, &barrier);

        if (dfn.vkEndCommandBuffer(swap_command_buffer_) != VK_SUCCESS) {
          return false;
        }

        VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &swap_command_buffer_;
        VkResult result;
        {
          const auto queue_acquisition =
              vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
          result = dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, swap_fence_);
        }
        if (result != VK_SUCCESS) {
          MANTRA_ERROR("swap upload submission failed ({})", int32_t(result));
          dfn.vkResetCommandPool(vulkan_device_->device(), swap_command_pool_, 0);
          return false;
        }
        swap_fence_pending_ = true;
        return true;
      });
  if (presented) {
    ++swap_stats_.presented;
  } else {
    ++swap_stats_.refresh_failed;
    MANTRA_WARN_ONCE("presenter refresh failed for a {}x{} guest output", width, height);
  }
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
  MANTRA_INFO("shaders: {} loads, {} unique, {} dumped; cache {} exact, {} normalized, "
              "{} failed, {} miss",
              shader_stats_.loads, shader_stats_.unique, shader_stats_.dumped,
              shader_stats_.cache_exact, shader_stats_.cache_normalized,
              shader_stats_.cache_failed, shader_stats_.cache_miss);
  swap_stats_.last_report_time = now;
  swap_stats_.swaps_at_last_report = swap_stats_.swaps;
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
  if (shader_hashes_seen_.insert(hash).second) {
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
  return nullptr;
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

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) {
  return true;
}

bool NativeCommandProcessor::IssueCopy() {
  return true;
}

}  // namespace rex::graphics::native
