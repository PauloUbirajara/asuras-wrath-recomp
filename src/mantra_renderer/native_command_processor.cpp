#include "native_command_processor.h"

#include <array>
#include "memexport.h"
#include "mantra_log.h"
#include "shader_key.h"
#include "../platform/platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <numeric>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
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

namespace mantra {
namespace {
std::mutex g_trace_file_mutex;
std::FILE* g_trace_file = nullptr;
}  // namespace

void TraceLine(std::string line) {
  {
    std::lock_guard<std::mutex> lock(g_trace_file_mutex);
    if (g_trace_file) {
      line += '\n';
      std::fwrite(line.data(), 1, line.size(), g_trace_file);
      return;
    }
  }
  MANTRA_INFO("trace: {}", line);
}

namespace {
constexpr size_t kTracesKept = 30;  // the oldest go when a new one starts

// Opens <MANTRA_TRACE_DIR>/<date>_<time>_frame<N>.log for the traced frame's
// lines. Returns the path, or empty (no folder: they stay in the log).
std::filesystem::path BeginTraceFile(uint64_t frame) {
  const char* dir_env = std::getenv("MANTRA_TRACE_DIR");
  if (!dir_env || !*dir_env) return {};
  namespace fs = std::filesystem;
  const fs::path dir(dir_env);
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::vector<fs::path> old;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".log" &&
        entry.path().filename().string().find("_frame") != std::string::npos) {
      old.push_back(entry.path());
    }
  }
  std::sort(old.begin(), old.end());  // the names start with the date
  for (size_t i = 0; i + kTracesKept <= old.size(); ++i) {
    fs::remove(old[i], ec);
    fs::remove(fs::path(old[i]).replace_extension(".png"), ec);  // its picture
  }
  const std::time_t now = std::time(nullptr);
  char when[32] = "trace";
  if (const std::tm* tm = std::localtime(&now)) {
    std::strftime(when, sizeof(when), "%Y-%m-%d_%H-%M-%S", tm);
  }
  const fs::path path = dir / fmt::format("{}_frame{}.log", when, frame);
  std::lock_guard<std::mutex> lock(g_trace_file_mutex);
  if (g_trace_file) std::fclose(g_trace_file);
  g_trace_file = std::fopen(path.string().c_str(), "w");
  if (!g_trace_file) return {};
  std::fprintf(g_trace_file, "mantra trace of frame %llu, %s\n",
               static_cast<unsigned long long>(frame), when);
  return path;
}

void EndTraceFile() {
  std::lock_guard<std::mutex> lock(g_trace_file_mutex);
  if (g_trace_file) std::fclose(g_trace_file);
  g_trace_file = nullptr;
}
}  // namespace
}  // namespace mantra

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

  InitializeGpuProfiler();
  if (!copy_pipelines_.Initialize(vulkan_device_) || !render_targets_.Initialize(vulkan_device_) ||
      !shader_interface_.Initialize(vulkan_device_, kFrameSlots) ||
      !draw_pipelines_.Initialize(vulkan_device_, shader_interface_.pipeline_layout()) ||
      !buffer_cache_.Initialize(vulkan_device_, memory_) ||
      !texture_cache_.Initialize(vulkan_device_, &buffer_cache_, &resolved_surfaces_)) {
    DestroyFrameResources();
    return false;
  }
  resolved_surfaces_.Initialize(vulkan_device_);

  // Offline-translated shaders (mantra_shadergen). Optional for now: nothing
  // draws yet, lookups only feed the stats.
  const std::filesystem::path cache_path = mantra::ShaderCache::DefaultPath();
  shader_cache_path_ = cache_path;
  {
    std::error_code ec;
    shader_cache_time_ = std::filesystem::last_write_time(cache_path, ec);
    shader_cache_bytes_ = std::filesystem::file_size(cache_path, ec);
    if (ec) shader_cache_bytes_ = 0;
  }
  if (!cache_path.empty() && shader_cache_.Load(cache_path)) {
    MANTRA_INFO("shader cache: {} entries from {}", shader_cache_.size(), cache_path.string());
    if (mantra::RenderScale8() != 8 && shader_cache_.version() < 8) {
      MANTRA_WARN("shader cache v{} predates the render scale: rebuild it (screen-space effects "
                  "will be misplaced)", shader_cache_.version());
    }
  } else {
    MANTRA_WARN("no shader cache at {}; run mantra_shadergen --game <extracted> --out <path>",
                cache_path.string());
  }
  if (mantra::RenderScale8() != 8) {
    MANTRA_INFO("render scale {}x ({}/8, MANTRA_RENDER_SCALE)", mantra::RenderScale8() / 8.0,
                mantra::RenderScale8());
  }
  StartPipelineWarmUp();
  return true;
}

// --- Pipelines from earlier runs ---------------------------------------------

namespace {
std::filesystem::path PipelineDir() {
  if (const char* dir = std::getenv("MANTRA_PIPELINE_DIR"); dir && *dir) return dir;
  const std::filesystem::path base = asura::platform::CacheDir();
  return base.empty() ? base : base / "pipelines";
}
}  // namespace

void NativeCommandProcessor::StartPipelineWarmUp() {
  const std::filesystem::path dir = PipelineDir();
  if (dir.empty()) {
    return;
  }
  VkPhysicalDeviceProperties properties;
  vulkan_device_->vulkan_instance()->functions().vkGetPhysicalDeviceProperties(
      vulkan_device_->physical_device(), &properties);
  std::string uuid;
  for (uint8_t b : properties.pipelineCacheUUID) uuid += fmt::format("{:02x}", b);
  // Keys per GPU model (formats depend on it); driver cache per driver build.
  draw_pipelines_.OpenCache(dir / fmt::format("{}.vkcache", uuid));
  std::vector<mantra::StoredShader> shaders;
  std::vector<mantra::StoredPipeline> pipelines;
  const auto path =
      dir / fmt::format("{:04x}_{:04x}.mpso", properties.vendorID, properties.deviceID);
  if (!pipeline_store_.Open(path, shaders, pipelines)) {
    return;
  }
  const char* env = std::getenv("MANTRA_PIPELINE_WARMUP");
  if (env && *env == '0') {
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  for (const mantra::StoredShader& s : shaders) {
    LoadShader(s.is_pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, 0,
               s.ucode.data(), uint32_t(s.ucode.size()));
  }
  auto module = [&](int type, uint64_t hash) -> uint64_t {
    auto it = shaders_[type].find(hash);
    if (it == shaders_[type].end()) return 0;
    return uint64_t(static_cast<MantraShader*>(it->second.get())->module);
  };
  std::vector<mantra::PipelineKey> keys;
  keys.reserve(pipelines.size());
  for (const mantra::StoredPipeline& p : pipelines) {
    mantra::PipelineKey key;
    std::memcpy(&key, &p.key, sizeof(key));
    key.vertex_module = module(0, p.vs);
    key.pixel_module = p.ps ? module(1, p.ps) : 0;
    if (!key.vertex_module || (p.ps && !key.pixel_module)) continue;
    keys.push_back(key);
  }
  MANTRA_INFO("pipeline store {}: {} shaders ({:.0f} ms to load), {} pipelines", path.string(),
              shaders.size(),
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                  .count(),
              keys.size());
  draw_pipelines_.WarmUp(std::move(keys));
}

void NativeCommandProcessor::RecordPipeline(const MantraShader& vs, const MantraShader* ps,
                                            const mantra::PipelineKey& key) {
  if (!pipeline_store_.is_open()) {
    return;
  }
  // Back to the raw big-endian microcode the hashes are over.
  auto add = [&](const MantraShader& shader) {
    const auto& ucode = shader.ucode_data();
    std::vector<uint32_t> raw(ucode.size());
    for (size_t i = 0; i < ucode.size(); ++i) raw[i] = __builtin_bswap32(ucode[i]);
    if (XXH3_64bits(raw.data(), raw.size() * sizeof(uint32_t)) != shader.ucode_data_hash()) {
      MANTRA_WARN_ONCE("pipeline store: shader {:016X} doesn't match its hash, not stored",
                       shader.ucode_data_hash());
      return false;
    }
    pipeline_store_.AddShader(shader.type() == xenos::ShaderType::kPixel, shader.ucode_data_hash(),
                              raw.data(), raw.size());
    return true;
  };
  if (!add(vs) || (ps && !add(*ps))) {
    return;
  }
  pipeline_store_.AddPipeline(vs.ucode_data_hash(), ps ? ps->ucode_data_hash() : 0, key);
}

void NativeCommandProcessor::ShutdownContext() {
  ReportDrawCensus();
  DestroyFrameResources();
}


namespace {
// Adds the scope's wall time to a nanosecond counter.
struct ScopedNs {
  uint64_t& acc;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~ScopedNs() {
    acc += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count());
  }
};

// CPU time of the calling thread, in nanoseconds.
uint64_t ThreadCpuNs() { return asura::platform::ThreadCpuNs(); }

// Run time of every thread of the process (platform::SampleThreads,
// nanoseconds), to name the threads that were busy over an interval.
struct ThreadTimes {
  struct Entry {
    std::string name;
    uint64_t ns = 0;
  };
  std::unordered_map<int, Entry> threads;

  static ThreadTimes Sample() {
    ThreadTimes out;
    for (auto& t : asura::platform::SampleThreads()) {
      out.threads.emplace(t.id, Entry{std::move(t.name), t.ns});
    }
    return out;
  }

  // "name(tid) N ms" for the `count` busiest threads since `before`, with the
  // share of `wall_ms` each used.
  std::string Busiest(const ThreadTimes& before, double wall_ms, size_t count) const {
    std::vector<std::pair<double, std::string>> busy;
    for (const auto& [tid, entry] : threads) {
      const auto it = before.threads.find(tid);
      const uint64_t prev = it != before.threads.end() ? it->second.ns : 0;
      const double ms = double(entry.ns - std::min(entry.ns, prev)) / 1e6;
      if (ms >= 0.5) busy.emplace_back(ms, fmt::format("{}({})", entry.name, tid));
    }
    std::sort(busy.begin(), busy.end(), std::greater<>());
    std::string out;
    for (size_t i = 0; i < std::min(count, busy.size()); ++i) {
      out += fmt::format("{}{} {:.0f} ms ({:.0f}%)", i ? ", " : "", busy[i].second,
                         busy[i].first, 100.0 * busy[i].first / std::max(wall_ms, 1e-3));
    }
    return out;
  }
};

// MANTRA_THREAD_PROFILE=1: sample all threads every frame so hitches name the
// busy threads (costs ~0.3 ms per frame). The 5 s report samples regardless.
bool ThreadProfileEveryFrame() {
  static const bool enabled = [] {
    const char* env = std::getenv("MANTRA_THREAD_PROFILE");
    return env && *env && *env != '0';
  }();
  return enabled;
}
// Snapshots for the hitch log (every frame, if enabled) and the 5 s report
// (both taken on the GPU thread).
ThreadTimes g_frame_threads, g_window_threads;
std::chrono::steady_clock::time_point g_window_threads_time{};
}  // namespace

// --- GPU profiling -----------------------------------------------------------

void NativeCommandProcessor::InitializeGpuProfiler() {
  GpuProfiler& p = gpu_profiler_;
  const char* env = std::getenv("MANTRA_GPU_PROFILE");
  if (!env || !*env || !std::strcmp(env, "0")) {
    return;
  }
  const int mode = !std::strcmp(env, "pass") ? 1 : 2;
  const auto& ifn = vulkan_device_->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device_->physical_device();
  VkPhysicalDeviceProperties properties;
  ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);
  uint32_t family_count = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
  const uint32_t family = vulkan_device_->queue_family_graphics_compute();
  const uint32_t valid_bits = family < family_count ? families[family].timestampValidBits : 0;
  const VkDevice device = vulkan_device_->device();
  p.write_timestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
      ifn.vkGetDeviceProcAddr(device, "vkCmdWriteTimestamp"));
  p.get_results = reinterpret_cast<PFN_vkGetQueryPoolResults>(
      ifn.vkGetDeviceProcAddr(device, "vkGetQueryPoolResults"));
  if (!valid_bits || !p.write_timestamp || !p.get_results) {
    MANTRA_WARN("gpu profile: timestamps unsupported on this queue, profiling off");
    return;
  }
  p.valid_mask = valid_bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << valid_bits) - 1;
  p.ns_per_tick = properties.limits.timestampPeriod;
  const auto& dfn = vulkan_device_->functions();
  VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = kGpuQueriesPerSlot;
  for (uint32_t i = 0; i < kFrameSlots; ++i) {
    if (dfn.vkCreateQueryPool(device, &info, nullptr, &p.pools[i]) != VK_SUCCESS) {
      MANTRA_WARN("gpu profile: query pool creation failed, profiling off");
      for (VkQueryPool& pool : p.pools) {
        if (pool) dfn.vkDestroyQueryPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
      }
      return;
    }
    p.marks[i].reserve(kGpuQueriesPerSlot);
  }
  p.mode = mode;
  MANTRA_INFO("gpu profile: {} mode on {} ({:.2f} ns/tick, {} valid bits)",
              mode == 1 ? "pass" : "draw", properties.deviceName, p.ns_per_tick, valid_bits);
}

void NativeCommandProcessor::GpuMarkHere(GpuMarkKind kind, uint64_t a, uint64_t b, uint32_t n,
                                         uint32_t extra) {
  GpuProfiler& p = gpu_profiler_;
  if (!p.mode || !recording_) {
    return;
  }
  const uint32_t index = uint32_t(submission_current_ % kFrameSlots);
  std::vector<GpuMark>& marks = p.marks[index];
  if (marks.size() >= kGpuQueriesPerSlot) {
    ++p.overflows;
    return;
  }
  p.write_timestamp(CurrentSlot().command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    p.pools[index], uint32_t(marks.size()));
  marks.push_back({kind, a, b, n, extra});
}

void NativeCommandProcessor::CollectGpuProfile(uint32_t slot_index) {
  GpuProfiler& p = gpu_profiler_;
  std::vector<GpuMark>& marks = p.marks[slot_index];
  if (!p.mode || marks.size() < 2) {
    marks.clear();
    return;
  }
  std::vector<uint64_t> ticks(marks.size());
  const VkResult result = p.get_results(
      vulkan_device_->device(), p.pools[slot_index], 0, uint32_t(marks.size()),
      ticks.size() * sizeof(uint64_t), ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
  if (result == VK_SUCCESS) {
    uint64_t since_setup = 0;
    for (size_t i = 1; i < marks.size(); ++i) {
      const GpuMark& m = marks[i];
      const uint64_t delta_ticks = (ticks[i] - ticks[i - 1]) & p.valid_mask;
      const uint64_t ns = uint64_t(double(delta_ticks) * p.ns_per_tick);
      p.kind_ns[size_t(m.kind)] += ns;
      if (m.kind == GpuMarkKind::kSetup) {
        since_setup = 0;
        continue;
      }
      since_setup += ns;
      // Setup / tail / present are only totals; the rest per label.
      if (m.kind == GpuMarkKind::kTail || m.kind == GpuMarkKind::kStart) continue;
      GpuProfileEntry& e = p.entries[{uint8_t(m.kind), m.a, m.b}];
      e.ns += ns;
      e.count += 1;
      e.n += m.n;
      e.extra = m.extra;
      if (m.kind == GpuMarkKind::kPass) {
        e.inclusive_ns += since_setup;
        since_setup = 0;
      }
    }
  }
  marks.clear();
}

void NativeCommandProcessor::ReportGpuProfile(double frames) {
  GpuProfiler& p = gpu_profiler_;
  if (!p.mode) {
    return;
  }
  frames = std::max(frames, 1.0);
  const auto per_frame = [&](uint64_t ns) { return double(ns) / 1e6 / frames; };
  const auto kind = [&](GpuMarkKind k) { return per_frame(p.kind_ns[size_t(k)]); };
  double busy = 0;
  for (uint64_t ns : p.kind_ns) busy += per_frame(ns);
  busy -= kind(GpuMarkKind::kStart);
  MANTRA_INFO("gpu profile: {:.1f} ms busy per frame: passes {:.1f} (draws {:.1f}, ends {:.1f}), "
              "pass setup {:.1f}, fills {:.1f}, resolves {:.1f}, present {:.1f}, tail {:.1f}; "
              "{} marks dropped",
              busy, kind(GpuMarkKind::kDraw) + kind(GpuMarkKind::kPass),
              kind(GpuMarkKind::kDraw), kind(GpuMarkKind::kPass), kind(GpuMarkKind::kSetup),
              kind(GpuMarkKind::kFill), kind(GpuMarkKind::kResolve), kind(GpuMarkKind::kPresent),
              kind(GpuMarkKind::kTail), p.overflows);
  using Item = std::pair<const std::tuple<uint8_t, uint64_t, uint64_t>*, const GpuProfileEntry*>;
  std::vector<Item> passes, draws, others;
  for (const auto& [key, entry] : p.entries) {
    const auto k = GpuMarkKind(std::get<0>(key));
    (k == GpuMarkKind::kPass ? passes : k == GpuMarkKind::kDraw ? draws : others)
        .push_back({&key, &entry});
  }
  const auto by_cost = [](const Item& x, const Item& y) {
    return x.second->ns + x.second->inclusive_ns > y.second->ns + y.second->inclusive_ns;
  };
  std::sort(passes.begin(), passes.end(), by_cost);
  std::sort(draws.begin(), draws.end(), by_cost);
  std::sort(others.begin(), others.end(), by_cost);
  for (size_t i = 0; i < std::min<size_t>(passes.size(), 10); ++i) {
    const auto& [key, e] = passes[i];
    const uint64_t a = std::get<1>(*key), b = std::get<2>(*key);
    MANTRA_INFO("gpu profile:   pass {:.2f} ms ({:.2f}/frame x{:.1f}, end {:.2f}): rt base {} "
                "pitch {} msaa {} colors {} vkfmt {} depth {} {}x{}, {:.0f} draws/frame",
                per_frame(e->inclusive_ns), per_frame(e->inclusive_ns) / std::max(1.0, double(e->count) / frames),
                double(e->count) / frames, per_frame(e->ns), a & 0xFFFF, (a >> 16) & 0xFFFF,
                1u << ((a >> 32) & 0xFF), (a >> 40) & 0xFF, b >> 32,
                (b & 0x10000) ? fmt::format("{}", b & 0xFFFF) : std::string("none"),
                e->extra >> 16, e->extra & 0xFFFF, double(e->n) / frames);
  }
  for (size_t i = 0; i < std::min<size_t>(draws.size(), 16); ++i) {
    const auto& [key, e] = draws[i];
    MANTRA_INFO("gpu profile:   draw {:.2f} ms ({:.0f}/frame, {:.0f} verts/frame): vs {:016X} ps "
                "{:016X} rt base {} vkfmt {}",
                per_frame(e->ns), double(e->count) / frames, double(e->n) / frames,
                std::get<1>(*key), std::get<2>(*key), e->extra & 0xFFFF, e->extra >> 16);
  }
  for (size_t i = 0; i < std::min<size_t>(others.size(), 6); ++i) {
    const auto& [key, e] = others[i];
    static const char* const kNames[] = {"start", "setup", "pass", "draw",
                                         "fill",  "resolve", "present", "tail"};
    MANTRA_INFO("gpu profile:   {} {:.2f} ms ({:.1f}/frame): {:X} {:X}",
                kNames[std::get<0>(*key)], per_frame(e->ns), double(e->count) / frames,
                std::get<1>(*key), std::get<2>(*key));
  }
  p.entries.clear();
  std::fill(std::begin(p.kind_ns), std::end(p.kind_ns), 0);
  p.overflows = 0;
}

// --- Submissions -------------------------------------------------------------

void NativeCommandProcessor::AwaitSlot(FrameSlot& slot) {
  if (!slot.pending) {
    return;
  }
  const auto& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  {
    ScopedNs timer{frame_timing_.fence_ns};
    dfn.vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
  }
  dfn.vkResetFences(device, 1, &slot.fence);
  slot.pending = false;
  CollectGpuProfile(uint32_t(&slot - frame_slots_));
  submission_completed_ = std::max(submission_completed_, slot.submission);
  buffer_cache_.SubmissionCompleted(submission_completed_);
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
  if (gpu_profiler_.mode) {
    const uint32_t index = uint32_t(submission_current_ % kFrameSlots);
    gpu_profiler_.marks[index].clear();
    dfn.vkCmdResetQueryPool(slot.command_buffer, gpu_profiler_.pools[index], 0,
                            kGpuQueriesPerSlot);
    GpuMarkHere(GpuMarkKind::kStart);
  }
  shader_interface_.BeginSlot(uint32_t(submission_current_ % kFrameSlots));
  shader_interface_.InitializeResources(slot.command_buffer);
  return slot.command_buffer;
}

bool NativeCommandProcessor::SubmitCommands() {
  if (!recording_) {
    return true;
  }
  EndRendering();
  GpuMarkHere(GpuMarkKind::kTail);
  recording_ = false;
  FrameSlot& slot = CurrentSlot();
  const auto& dfn = vulkan_device_->functions();
  shader_interface_.FlushSlot();
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

mantra::RenderTarget* NativeCommandProcessor::GetColorTarget(
    VkCommandBuffer cmd, uint32_t base_tiles, uint32_t pitch_px, xenos::MsaaSamples msaa,
    xenos::ColorRenderTargetFormat format, uint64_t frame) {
  mantra::RenderTarget* from = nullptr;
  mantra::RenderTarget* rt =
      render_targets_.GetColor(cmd, base_tiles, pitch_px, msaa, format, frame, &from);
  if (!rt || !from) {
    return rt;
  }
  if (msaa != xenos::MsaaSamples::k1X) {
    MANTRA_WARN_ONCE("7e3 <-> 32bpp EDRAM reinterpretation with MSAA is not implemented");
    return rt;
  }
  // Never submit here: the caller keeps using `cmd`.
  if (!recording_ || CurrentSlot().descriptor_sets_used >= kDescriptorSetsPerSlot) {
    MANTRA_WARN_ONCE("no descriptor set left for a 7e3 conversion, skipping it");
    return rt;
  }
  ++CurrentSlot().descriptor_sets_used;
  const bool to_32 = rt->key.cls == mantra::RtClass::kColor32;
  if (TracingThisFrame()) {
    MANTRA_FRAME_TRACE("convert 7e3 {} 32bpp at base {} pitch {} (fmt {}, from fmt {})",
                to_32 ? "->" : "<-", base_tiles, pitch_px, uint32_t(format),
                uint32_t(from->last_format));
  }
  const VkFormat float_format = VK_FORMAT_R16G16B16A16_SFLOAT;
  const VkFormat bits_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  const VkFormat src_format = to_32 ? float_format : bits_format;
  const VkFormat dst_format = to_32 ? bits_format : float_format;
  const VkImageView src_view = render_targets_.GetView(*from, src_format);
  const VkImageView dst_view = render_targets_.GetView(*rt, dst_format);
  if (!src_view || !dst_view) {
    return rt;
  }
  mantra::TransitionImage(vulkan_device_, cmd, from->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  mantra::TransitionImage(vulkan_device_, cmd, rt->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          mantra::kColorAttachmentStage, mantra::kColorAttachmentAccess);
  mantra::CopyRect rect;
  rect.width = std::min(rt->image.width, from->image.width);
  rect.height = std::min(rt->image.height, from->image.height);
  mantra::CopyPushConstants constants;
  constants.mode = to_32 ? 1 : 2;
  if (copy_pipelines_.Record(cmd, CurrentSlot().descriptor_pool, src_view, false, dst_view,
                             dst_format, rt->image.width, rt->image.height, rect, constants)) {
    ++draw_stats_.float7e3_conversions;
    if (FirstRoute((uint64_t(base_tiles) << 40) ^ (uint64_t(pitch_px) << 20) ^ (to_32 ? 7 : 3) ^
                   (uint64_t(0x7E3) << 52))) {
      MANTRA_INFO("edram: 7e3 {} 32bpp at base {} pitch {} ({}x{})", to_32 ? "->" : "<-",
                  base_tiles, pitch_px, rect.width, rect.height);
    }
  }
  return rt;
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
  draw_pipelines_.Shutdown();
  texture_cache_.Shutdown();
  buffer_cache_.Shutdown();
  DestroyShaderModules();
  shader_interface_.Shutdown();
  const auto& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  for (FrameSlot& slot : frame_slots_) {
    if (slot.descriptor_pool) dfn.vkDestroyDescriptorPool(device, slot.descriptor_pool, nullptr);
    if (slot.fence) dfn.vkDestroyFence(device, slot.fence, nullptr);
    // Frees the command buffer too.
    if (slot.command_pool) dfn.vkDestroyCommandPool(device, slot.command_pool, nullptr);
    slot = FrameSlot{};
  }
  for (VkQueryPool& pool : gpu_profiler_.pools) {
    if (pool) dfn.vkDestroyQueryPool(device, pool, nullptr);
    pool = VK_NULL_HANDLE;
  }
  gpu_profiler_.mode = 0;
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

// --- Constant write tracing -------------------------------------------------

namespace {
constexpr uint32_t kPsConstFirst = XE_GPU_REG_SHADER_CONSTANT_256_X;
constexpr uint32_t kPsConstEnd = kPsConstFirst + 32 * 4;  // c0..c31
}  // namespace

void NativeCommandProcessor::TracePsConstantWrite(uint32_t start_index, const uint32_t* values,
                                                  uint32_t count, const void* source) {
  std::string s;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t r = start_index + i;
    if (r < kPsConstFirst || r >= kPsConstEnd) continue;
    const uint32_t c = r - kPsConstFirst;
    float f;
    std::memcpy(&f, &values[i], 4);
    if (s.empty() || c % 4 == 0) s += fmt::format(" c{}.{}=", c / 4, "xyzw"[c % 4]);
    s += fmt::format("{:g}{}", f, c % 4 == 3 ? "" : ",");
  }
  const auto* host = static_cast<const uint8_t*>(source);
  const uint8_t* membase = memory_->physical_membase();
  std::string from = "reg";
  if (host && host >= membase && host < membase + 0x20000000) {
    from = fmt::format("{:08X}", uint32_t(host - membase));
  } else if (host) {
    from = fmt::format("v{:08X}", memory_->HostToGuestVirtual(host));
  }
  MANTRA_FRAME_TRACE("ps const write {} regs from {}:{}", count, from, s);
}

void NativeCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  if (!in_register_range_write_ && index - kPsConstFirst < kPsConstEnd - kPsConstFirst &&
      trace_swap_ && TracingThisFrame()) {
    TracePsConstantWrite(index, &value, 1, nullptr);
  }
  CommandProcessor::WriteRegister(index, value);
}

void NativeCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                   uint32_t num_registers) {
  if (trace_swap_ && start_index < kPsConstEnd && start_index + num_registers > kPsConstFirst &&
      TracingThisFrame()) {
    std::vector<uint32_t> values(num_registers);
    for (uint32_t i = 0; i < num_registers; ++i) {
      values[i] = rex::memory::load_and_swap<uint32_t>(base + i);
    }
    TracePsConstantWrite(start_index, values.data(), num_registers, base);
  }
  in_register_range_write_ = true;
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
  in_register_range_write_ = false;
}

// Debug: MANTRA_PS_CONST_OVERRIDE="<ps hash>:<c>=x,y,z,w;..." replaces pixel
// shader float constant c (0..31) for draws with that pixel shader; the
// register file is restored after the draw.
namespace {
struct PsConstOverride {
  uint64_t hash;
  uint32_t constant;
  float value[4];
};
const std::vector<PsConstOverride>& PsConstOverrides() {
  static const std::vector<PsConstOverride> rules = [] {
    std::vector<PsConstOverride> out;
    const char* env = std::getenv("MANTRA_PS_CONST_OVERRIDE");
    for (const char* p = env; p && *p;) {
      PsConstOverride rule{};
      char* end;
      rule.hash = std::strtoull(p, &end, 16);
      if (*end != ':') break;
      rule.constant = uint32_t(std::strtoul(end + 1, &end, 10));
      if (*end != '=' || rule.constant >= 32) break;
      p = end + 1;
      for (int i = 0; i < 4; ++i) {
        rule.value[i] = std::strtof(p, &end);
        p = *end == ',' ? end + 1 : end;
      }
      out.push_back(rule);
      MANTRA_INFO("PS constant override: {:016X} c{} = ({:g} {:g} {:g} {:g})", rule.hash,
                  rule.constant, rule.value[0], rule.value[1], rule.value[2], rule.value[3]);
      p = std::strchr(p, ';');
      p = p ? p + 1 : nullptr;
    }
    return out;
  }();
  return rules;
}
}  // namespace

// --- Draws and resolves ------------------------------------------------------

bool NativeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info, bool) {
  ScopedNs timer{frame_timing_.draw_ns};
  // Debug constant overrides, restored when the draw returns.
  struct SavedConstant {
    uint32_t reg;
    uint32_t value[4];
  };
  std::vector<SavedConstant> saved_constants;
  if (const auto& rules = PsConstOverrides(); !rules.empty()) {
    auto* ps = static_cast<MantraShader*>(active_pixel_shader());
    if (ps && ps->cache_entry) {
      for (const PsConstOverride& rule : rules) {
        if (rule.hash != ps->cache_entry->exact_hash) continue;
        const uint32_t reg = kPsConstFirst + rule.constant * 4;
        SavedConstant saved{reg, {}};
        for (uint32_t i = 0; i < 4; ++i) {
          saved.value[i] = (*register_file_)[reg + i];
          std::memcpy(&(*register_file_)[reg + i], &rule.value[i], 4);
        }
        saved_constants.push_back(saved);
      }
    }
  }
  struct RestoreConstants {
    RegisterFile& regs;
    std::vector<SavedConstant>& saved;
    ~RestoreConstants() {
      for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
        for (uint32_t i = 0; i < 4; ++i) regs[it->reg + i] = it->value[i];
      }
    }
  } restore_constants{*register_file_, saved_constants};
  // Debug: MANTRA_SKIP_PS_CONST="<ps hash>:<c>=<x>" drops draws with that
  // pixel shader whose float constant c.x is x (to within 1e-3), e.g. one
  // light's pass picked out by its colour.
  static const std::optional<PsConstOverride> skip_rule = []() -> std::optional<PsConstOverride> {
    const char* env = std::getenv("MANTRA_SKIP_PS_CONST");
    if (!env || !*env) return std::nullopt;
    PsConstOverride rule{};
    char* end;
    rule.hash = std::strtoull(env, &end, 16);
    if (*end != ':') return std::nullopt;
    rule.constant = uint32_t(std::strtoul(end + 1, &end, 10));
    if (*end != '=' || rule.constant >= 32) return std::nullopt;
    rule.value[0] = std::strtof(end + 1, nullptr);
    MANTRA_INFO("skipping draws of PS {:016X} with c{}.x = {:g}", rule.hash, rule.constant,
                rule.value[0]);
    return rule;
  }();
  if (skip_rule) {
    auto* ps = static_cast<MantraShader*>(active_pixel_shader());
    if (ps && ps->cache_entry && ps->cache_entry->exact_hash == skip_rule->hash &&
        std::abs(register_file_->Get<float>(kPsConstFirst + skip_rule->constant * 4) -
                 skip_rule->value[0]) < 1e-3f) {
      if (TracingThisFrame()) MANTRA_FRAME_TRACE("skipped by MANTRA_SKIP_PS_CONST");
      return true;
    }
  }

  const xenos::EdramMode edram_mode = register_file_->Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Not a draw: the RB copies EDRAM to memory, the shaders never run.
    ++draw_stats_.copy_draws;
    if (TracingThisFrame()) {
      const auto& regs = *register_file_;
      MANTRA_FRAME_TRACE("copy rt src {} -> {:08X} fmt {} clear {}",
                  regs.Get<reg::RB_COPY_CONTROL>().copy_src_select,
                  regs[XE_GPU_REG_RB_COPY_DEST_BASE],
                  uint32_t(regs.Get<reg::RB_COPY_DEST_INFO>().copy_dest_format),
                  regs.Get<reg::RB_COPY_CONTROL>().color_clear_enable);
    }
    return IssueCopy();
  }
  ++draw_stats_.draws;
  // Memexport (VS writing to memory, e.g. the game's GPU memcpy): run it on
  // the CPU. Such draws have no color writes, so nothing else is drawn.
  if (auto* mvs = static_cast<MantraShader*>(active_vertex_shader());
      mvs && mvs->memexport_eM_written()) {
    uint32_t lo = UINT32_MAX, hi = 0;
    mantra::MemExportIndices indices;
    if (index_buffer_info) {
      indices.guest_base = index_buffer_info->guest_base;
      indices.count = index_buffer_info->count;
      indices.is_32bit = index_buffer_info->format == xenos::IndexFormat::kInt32;
      indices.endianness = index_buffer_info->endianness;
    }
    ScopedNs memexport_timer{frame_timing_.phase_ns[5]};
    const bool ran = mantra::RunMemExportDraw(
        *register_file_, *memory_, *mvs, index_count, index_buffer_info ? &indices : nullptr,
        memexport_stats_,
        [&](uint32_t start, uint32_t length) {
          lo = std::min(lo, start);
          hi = std::max(hi, start + length);
        });
    if (hi > lo) buffer_cache_.NotifyGpuWrite(lo, hi - lo);
    if (TracingThisFrame()) {
      MANTRA_FRAME_TRACE("memexport vs {:016X} n {} ran {} wrote {:08X}..{:08X}",
                  mvs->ucode_data_hash(), index_count, ran, lo, hi);
    }
    if (ran && !register_file_->Get<reg::RB_COLOR_MASK>().value) return true;
  }
  const char* outcome = "fill";
  SolidFillResult fill;
  {
    ScopedNs fill_timer{frame_timing_.phase_ns[6]};
    fill = TrySolidFill(prim_type, edram_mode, index_count, index_buffer_info != nullptr);
  }
  if (fill == SolidFillResult::kNotSolidFill) {
    trace_current_draw_ = TracingThisFrame();
    const DrawResult result = DrawGuest(prim_type, index_count, index_buffer_info);
    trace_current_draw_ = false;
    outcome = result == DrawResult::kDrawn ? "drawn" : result == DrawResult::kSkipped ? "SKIPPED" : "FAILED";
    if (result == DrawResult::kDrawn) {
      ++gpu_profiler_.pass_draws;
      if (gpu_profiler_.mode == 2 && rendering_.active) {
        Shader* vs = active_vertex_shader();
        Shader* ps = active_pixel_shader();
        const mantra::RenderTarget* rt = rendering_.color[0] ? rendering_.color[0] : rendering_.depth;
        GpuMarkHere(GpuMarkKind::kDraw, vs ? vs->ucode_data_hash() : 0,
                    ps ? ps->ucode_data_hash() : 0, index_count,
                    (rt ? rt->key.base_tiles & 0xFFFF : 0xFFFF) |
                        (uint32_t(rendering_.color_format[0]) << 16));
      }
    }
  } else {
    GpuMarkHere(GpuMarkKind::kFill, (*register_file_)[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF);
  }
  // MANTRA_TRACE_PS=hash: also trace the first few draws with that pixel shader.
  static const uint64_t trace_ps = [] {
    const char* env = std::getenv("MANTRA_TRACE_PS");
    return env ? std::strtoull(env, nullptr, 16) : 0;
  }();
  if (trace_ps && !TracingThisFrame()) {
    static int traced = 0;
    auto* ps = static_cast<MantraShader*>(active_pixel_shader());
    if (traced < 4 && ps && ps->cache_entry && ps->cache_entry->exact_hash == trace_ps) {
      ++traced;
      TraceDraw(prim_type, index_count, outcome);
    }
  }
  if (TracingThisFrame()) {
    TraceDraw(prim_type, index_count, outcome);
    DumpDrawBuffers(index_count, index_buffer_info);
    if (CapturingDraws() && outcome[0] == 'd' && rendering_.active && rendering_.depth &&
        register_file_->Get<reg::RB_DEPTHCONTROL>().stencil_enable &&
        register_file_->Get<reg::RB_DEPTHCONTROL>().stencilzpass != xenos::StencilOp::kKeep) {
      mantra::RenderTarget& rt = *rendering_.depth;
      CaptureImage(rt.image, VK_FORMAT_S8_UINT, rt.image.width, std::min(rt.image.height, 720u),
                   0x688, fmt::format("s{}_pass_{:03}_stencil_{:016X}", trace_swap_,
                                      pass_capture_index_++,
                                      active_vertex_shader()->ucode_data_hash()));
      // The depth it was tested against (alpha column of the stats = raw depth).
      CaptureImage(rt.image,
                   rt.image.format == VK_FORMAT_D24_UNORM_S8_UINT ? VK_FORMAT_X8_D24_UNORM_PACK32
                                                                  : VK_FORMAT_D32_SFLOAT,
                   rt.image.width, std::min(rt.image.height, 720u), 0x688,
                   fmt::format("s{}_pass_{:03}_depth_{:016X}", trace_swap_,
                               pass_capture_index_++, active_vertex_shader()->ucode_data_hash()));
    }
    if (CapturingDraws() && outcome[0] == 'd' && rendering_.active && rendering_.color[0]) {
      mantra::RenderTarget& rt = *rendering_.color[0];
      CaptureImage(rt.image, rendering_.color_format[0], rt.image.width, rt.image.height,
                   0x688 /* RGBA */,
                   fmt::format("s{}_pass_{:03}_draw_rt{}_{:016X}", trace_swap_, pass_capture_index_++,
                               rt.key.base_tiles,
                               active_pixel_shader() ? active_pixel_shader()->ucode_data_hash()
                                                     : 0));
    }
  }
  // MANTRA_TRACE_VS=<hash>: trace the frame after the first draw with it.
  static const uint64_t trace_vs = [] {
    const char* env = std::getenv("MANTRA_TRACE_VS");
    return env ? std::strtoull(env, nullptr, 16) : 0;
  }();
  if (trace_vs && !trace_swap_ && active_vertex_shader() &&
      active_vertex_shader()->ucode_data_hash() == trace_vs) {
    trace_swap_ = swap_stats_.swaps + 2;
    MANTRA_INFO("trace: VS {:016X} seen at swap {}, tracing swap {}", trace_vs,
                swap_stats_.swaps, trace_swap_);
  }
  {
    Shader* vs = active_vertex_shader();
    Shader* ps = active_pixel_shader();
    ++draw_census_[{vs ? vs->ucode_data_hash() : 0, ps ? ps->ucode_data_hash() : 0,
                    uint32_t(edram_mode) | (uint32_t(prim_type) << 8)}];
  }
  return true;
}

bool NativeCommandProcessor::IssueCopy() {
  ScopedNs timer{frame_timing_.copy_ns};
  if (!Resolve()) {
    ++draw_stats_.resolves_failed;
  }
  GpuMarkHere(GpuMarkKind::kResolve, register_file_->Get<reg::RB_COPY_CONTROL>().copy_src_select,
              (*register_file_)[XE_GPU_REG_RB_COPY_DEST_BASE]);
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

  VkCommandBuffer cmd = BeginNonDrawCommands();
  if (!cmd) {
    return false;
  }
  const uint64_t frame = submission_current_;

  const auto color_info = regs.Get<reg::RB_COLOR_INFO>(
      reg::RB_COLOR_INFO::rt_register_indices[info.rb_copy_control.copy_src_select & 3]);

  // Copy.
  // Debug: MANTRA_NO_DEPTH_RESOLVE=1 leaves depth resolves unimplemented.
  static const bool no_depth_resolve = std::getenv("MANTRA_NO_DEPTH_RESOLVE") != nullptr;
  if (copying_depth && info.copy_dest_extent_length && !no_depth_resolve) {
    // Depth -> an R32_SFLOAT surface with the guest depth values; the
    // k_24_8 / k_24_8_FLOAT texture decoders copy from it (shadow maps).
    const auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    const bool depth_float = depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8;
    mantra::RenderTarget* rt =
        render_targets_.GetDepth(cmd, info.depth_original_base, pitch_px, msaa, frame);
    const VkImageView src_view = rt ? render_targets_.GetDepthSampleView(*rt) : VK_NULL_HANDLE;
    if (src_view) {
      const auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
      const uint32_t dest_address =
          regs[XE_GPU_REG_RB_COPY_DEST_BASE] & (kGuestPhysicalMemorySize - 1);
      rect.width = std::min<uint32_t>(rect.width, rt->image.width - std::min<uint32_t>(
                                                                         rect.x, rt->image.width));
      rect.height = std::min<uint32_t>(
          rect.height, rt->image.height - std::min<uint32_t>(rect.y, rt->image.height));
      mantra::ResolvedSurface* surface = resolved_surfaces_.Get(
          dest_address, std::max<uint32_t>(dest_pitch.copy_dest_pitch, rect.x + rect.width),
          std::max<uint32_t>(dest_pitch.copy_dest_height, rect.y + rect.height),
          info.copy_dest_info.copy_dest_format, frame, true);
      VkDescriptorPool descriptor_pool = ReserveDescriptorSet();
      cmd = BeginNonDrawCommands();
      if (surface && cmd) resolved_surfaces_.PrepareWrite(cmd, *surface);
      if (surface && descriptor_pool && cmd && rect.width && rect.height) {
        mantra::CopyPushConstants constants;
        constants.sample_mask = 1;  // depth: sample 0, no averaging
        // The host stores 20e4 depth halved (its range is [0, 2)).
        constants.scale = depth_float ? 2.0f : 1.0f;
        mantra::TransitionImage(vulkan_device_, cmd, rt->image,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        mantra::TransitionImage(vulkan_device_, cmd, surface->image,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                mantra::kColorAttachmentStage, mantra::kColorAttachmentAccess);
        if (copy_pipelines_.Record(cmd, descriptor_pool, src_view,
                                   msaa != xenos::MsaaSamples::k1X, surface->view,
                                   VK_FORMAT_R32_SFLOAT, surface->image.width,
                                   surface->image.height, rect, constants)) {
          ++draw_stats_.depth_copies;
          surface->write_count = ++resolve_serial_;  // unique: surfaces get recreated
          surface->last_write_frame = frame;
          texture_cache_.NoteResolve(dest_address, info.copy_dest_extent_length);
          if (TracingThisFrame() && CapturingPasses()) {
            // Also writes the raw floats (.f32) for offline shadow checks.
            CaptureImage(surface->image, surface->image.format, surface->image.width,
                         surface->image.height, 0x688 /* RGBA */,
                         fmt::format("s{}_pass_{:03}_depth_{:08X}_base{}_p{}", trace_swap_,
                                     pass_capture_index_++, dest_address,
                                     info.depth_original_base, pitch_px));
            cmd = BeginNonDrawCommands();
          }
        }
        if (FirstRoute((uint64_t(info.depth_original_base) << 40) ^ (uint64_t(pitch_px) << 26) ^
                       dest_address ^ (uint64_t(3) << 62)))
          MANTRA_DEBUG("resolve: depth base {} pitch {} fmt {} rect {},{} {}x{} -> 0x{:08X}",
                      info.depth_original_base, pitch_px, uint32_t(depth_info.depth_format),
                      rect.x, rect.y, rect.width, rect.height, dest_address);
      }
    } else {
      ++draw_stats_.depth_copies_skipped;
    }
  } else if (copying_depth) {
  } else if (info.copy_dest_extent_length) {
    const xenos::ColorFormat dest_format = info.copy_dest_info.copy_dest_format;
    const VkFormat dest_host_format = mantra::ResolvedSurfaces::HostFormat(dest_format);
    mantra::RenderTarget* rt = GetColorTarget(
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
      cmd = BeginNonDrawCommands();  // may have been submitted to free descriptors
      if (surface && cmd) resolved_surfaces_.PrepareWrite(cmd, *surface);
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
        // Fixed-point 16-bit targets are stored / 32 (their -32...32 range).
        // A float destination gets value * 2^exp_bias; a fixed one is hosted
        // as value / 32 too, and the game resolves it with exp_bias -5 into
        // -1...1, the same bits: both are the stored value * 32 * 2^exp_bias.
        if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
            color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
          constants.scale *= 32.0f;
        }
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
          surface->write_count = ++resolve_serial_;  // unique: surfaces get recreated
          surface->last_write_frame = frame;
          texture_cache_.NoteResolve(dest_address, info.copy_dest_extent_length);
          MaybeTraceOnResolve(dest_address, rect.width, rect.height);
          if (TracingThisFrame() && CapturingPasses()) {
            CaptureImage(surface->image, surface->image.format, surface->image.width,
                         surface->image.height, 0x688 /* RGBA */,
                         fmt::format("s{}_pass_{:03}_resolve_{:08X}_fmt{}", trace_swap_, pass_capture_index_++,
                                     dest_address, uint32_t(dest_format)));
            cmd = BeginNonDrawCommands();
          }
        }
        if (FirstRoute((uint64_t(info.color_original_base) << 40) ^ (uint64_t(pitch_px) << 26) ^
                       (uint64_t(color_info.color_format) << 22) ^ dest_address ^
                       (uint64_t(dest_format) << 52) ^ (uint64_t(1) << 63)))
          MANTRA_DEBUG(
            "resolve: rt base {} pitch {} fmt {} rect {},{} {}x{} -> 0x{:08X} fmt {} pitch {}{}",
            info.color_original_base, pitch_px, uint32_t(color_info.color_format), rect.x, rect.y,
            rect.width, rect.height, dest_address, uint32_t(dest_format),
            uint32_t(dest_pitch.copy_dest_pitch), info.IsClearingColor() ? " +clear" : "");
      }
    }
  }

  // Clears (after the copy, of the same rectangle).
  cmd = BeginNonDrawCommands();
  if (!cmd) {
    return false;
  }
  if (info.IsClearingColor()) {
    if (mantra::RenderTarget* rt = GetColorTarget(
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

bool NativeCommandProcessor::TraceCaptureEnabled() {
  int value = g_trace_capture.load(std::memory_order_relaxed);
  if (value < 0) {
    const char* env = std::getenv("MANTRA_TRACE_CAPTURE");
    value = !env || !*env || *env != '0';
    int unset = -1;
    g_trace_capture.compare_exchange_strong(unset, value);
  }
  return value != 0;
}

bool NativeCommandProcessor::CapturingPasses() {
  static const bool enabled = [] {
    const char* dir = std::getenv("MANTRA_CAPTURE_DIR");
    return dir && *dir;
  }();
  return enabled;
}

bool NativeCommandProcessor::CapturingDraws() {
  // MANTRA_CAPTURE_DRAWS=1: also dump the target after every traced draw
  // (slow: a traced frame then takes many seconds). Resolves always are.
  static const bool enabled = [] {
    const char* env = std::getenv("MANTRA_CAPTURE_DRAWS");
    return env && *env && *env != '0';
  }();
  if (!enabled || !CapturingPasses()) return false;
  // MANTRA_CAPTURE_DRAWS_PS=<hash>,...: only draws with these pixel shaders.
  static const std::vector<uint64_t> only_ps = [] {
    std::vector<uint64_t> out;
    for (const char* p = std::getenv("MANTRA_CAPTURE_DRAWS_PS"); p && *p;) {
      char* end;
      out.push_back(std::strtoull(p, &end, 16));
      p = *end == ',' ? end + 1 : nullptr;
    }
    return out;
  }();
  if (only_ps.empty()) return true;
  auto* ps = static_cast<MantraShader*>(active_pixel_shader());
  if (!ps) return false;
  for (uint64_t hash : only_ps) {
    if (hash == ps->ucode_data_hash() || (ps->cache_entry && hash == ps->cache_entry->exact_hash)) {
      return true;
    }
  }
  return false;
}

// MANTRA_TRACE_RESOLVE=<entry>[,...]: trace the frame after a color resolve
// matching an entry, up to MANTRA_TRACE_RESOLVE_COUNT times (default 3), at
// least 150 swaps apart; for scenes that are hard to time by hand with
// MANTRA_TRACE_TRIGGER. An entry is a hex destination address, or WxH (decimal)
// for the resolved rectangle's size (addresses change between runs).
void NativeCommandProcessor::MaybeTraceOnResolve(uint32_t address, uint32_t width,
                                                 uint32_t height) {
  struct Match {
    bool size;
    uint32_t a, b;
  };
  static const std::vector<Match> matches = [] {
    std::vector<Match> list;
    const char* env = std::getenv("MANTRA_TRACE_RESOLVE");
    for (const char* p = env; p && *p;) {
      const char* comma = std::strchr(p, ',');
      const std::string entry(p, comma ? size_t(comma - p) : std::strlen(p));
      p = comma ? comma + 1 : p + entry.size();
      if (const size_t x = entry.find('x'); x != std::string::npos && x > 0) {
        list.push_back({true, uint32_t(std::strtoul(entry.c_str(), nullptr, 10)),
                        uint32_t(std::strtoul(entry.c_str() + x + 1, nullptr, 10))});
      } else if (!entry.empty()) {
        list.push_back({false, uint32_t(std::strtoul(entry.c_str(), nullptr, 16)), 0});
      }
    }
    return list;
  }();
  if (matches.empty() ||
      std::none_of(matches.begin(), matches.end(), [&](const Match& m) {
        return m.size ? m.a == width && m.b == height : m.a == address;
      })) {
    return;
  }
  static const uint32_t max_traces = [] {
    const char* env = std::getenv("MANTRA_TRACE_RESOLVE_COUNT");
    return env ? uint32_t(std::strtoul(env, nullptr, 10)) : 3u;
  }();
  static uint32_t traces = 0;
  static uint64_t last_swap = 0;
  const uint64_t swaps = swap_stats_.swaps;
  if (traces >= max_traces || (trace_swap_ && trace_swap_ > swaps) ||
      (traces && swaps < last_swap + 150)) {
    return;
  }
  ++traces;
  last_swap = swaps;
  trace_swap_ = swaps + 2;
  MANTRA_FRAME_TRACE("resolve {}x{} to 0x{:08X} at swap {}, tracing swap {} ({}/{})", width,
              height, address, swaps, trace_swap_, traces, max_traces);
}

bool NativeCommandProcessor::TracingThisFrame() {
  // MANTRA_TRACE_SWAP=N logs every draw and copy before swap N.
  static const uint64_t env_swap = [] {
    const char* env = std::getenv("MANTRA_TRACE_SWAP");
    return env ? std::strtoull(env, nullptr, 10) : 0;
  }();
  if (env_swap && !trace_swap_) {
    trace_swap_ = env_swap;
  }
  return trace_swap_ && swap_stats_.swaps + 1 == trace_swap_;
}

void NativeCommandProcessor::DumpDrawBuffers(uint32_t index_count,
                                             const IndexBufferInfo* index_buffer_info) {
  const char* dir = std::getenv("MANTRA_CAPTURE_DIR");
  if (!dir || !*dir) return;
  // Each buffer once per traced frame; files are named by swap and address.
  static uint64_t dumped_swap = 0;
  static std::unordered_set<uint64_t> dumped;
  if (dumped_swap != trace_swap_) {
    dumped_swap = trace_swap_;
    dumped.clear();
  }
  const auto dump = [&](const char* kind, uint32_t address, uint32_t length) -> std::string {
    length = std::min<uint32_t>(length, 8u << 20);
    const std::string name =
        fmt::format("s{}_{}_{:08X}_{:X}.bin", trace_swap_, kind, address, length);
    if (!length || !dumped.insert(uint64_t(address) << 32 | length).second) return name;
    const auto* src = memory_->TranslatePhysical<const uint8_t*>(address);
    if (!src || uint64_t(address) + length > 0x20000000u) return name;
    if (std::FILE* file = std::fopen((std::filesystem::path(dir) / name).string().c_str(), "wb")) {
      std::fwrite(src, 1, length, file);
      std::fclose(file);
    }
    return name;
  };
  std::string files;
  if (index_buffer_info && index_buffer_info->guest_base) {
    const bool is_32 = index_buffer_info->format == xenos::IndexFormat::kInt32;
    files += fmt::format(" ib e{} {}", uint32_t(index_buffer_info->endianness),
                         dump("ib", index_buffer_info->guest_base & 0x1FFFFFFF,
                              index_count * (is_32 ? 4 : 2)));
  }
  if (auto* vs = static_cast<MantraShader*>(active_vertex_shader())) {
    std::unordered_set<uint32_t> fetches;
    for (const auto& in : vs->vertex_inputs) {
      if (!in.valid || !fetches.insert(in.fetch_constant).second) continue;
      const auto fetch = register_file_->GetVertexFetch(in.fetch_constant);
      files += fmt::format(" vf{} e{} {}", in.fetch_constant, uint32_t(fetch.endian),
                           dump("vb", (uint32_t(fetch.address) << 2) & 0x1FFFFFFF,
                                uint32_t(fetch.size) << 2));
    }
  }
  if (!files.empty()) MANTRA_FRAME_TRACE("  buffers{}", files);
  // All 256 bool constants (8 registers of 32): VS 0..127, PS 128..255.
  std::string bools;
  for (uint32_t i = 0; i < 8; ++i) {
    bools += fmt::format(" {:08X}", (*register_file_)[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i]);
  }
  MANTRA_FRAME_TRACE("  bools{}", bools);
}

void NativeCommandProcessor::TraceDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       const char* outcome) {
  const RegisterFile& regs = *register_file_;
  auto* vs = static_cast<MantraShader*>(active_vertex_shader());
  auto* ps = static_cast<MantraShader*>(active_pixel_shader());
  const auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
  const auto blend = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
  const auto depth = regs.Get<reg::RB_DEPTHCONTROL>();
  const auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  std::string textures;
  uint32_t mask = 0;
  if (vs && vs->cache_entry) mask |= vs->cache_entry->texture_slot_mask;
  if (ps && ps->cache_entry) mask |= ps->cache_entry->texture_slot_mask;
  while (mask) {
    const uint32_t slot = uint32_t(std::countr_zero(mask));
    mask &= mask - 1;
    const auto fetch = regs.GetTextureFetch(slot);
    textures += fmt::format(" t{}={:08X}/f{}/d{}/s{:o}", slot, fetch.base_address << 12,
                            uint32_t(fetch.format), uint32_t(fetch.dimension),
                            uint32_t(fetch.swizzle));
    if (fetch.exp_adjust) textures += fmt::format("/x{}", int32_t(fetch.exp_adjust));
    // Sign per component: 0 unsigned, 1 signed, 2 unsigned biased, 3 gamma.
    textures += fmt::format("/g{}{}{}{}", uint32_t(fetch.sign_x), uint32_t(fetch.sign_y),
                            uint32_t(fetch.sign_z), uint32_t(fetch.sign_w));
    textures += fmt::format("/w{:08X}.{:08X}.{:08X}.{:08X}.{:08X}.{:08X}", fetch.dword_0,
                            fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4,
                            fetch.dword_5);
    // Guest-memory content check: nonzero bytes in the first 64 KB.
    if (const auto* t = memory_->TranslatePhysical<const uint8_t*>((fetch.base_address << 12) &
                                                                   0x1FFFFFFF)) {
      uint32_t nonzero = 0;
      for (uint32_t i = 0; i < 65536; ++i) nonzero += t[i] != 0;
      textures += fmt::format("/nz{}/{}x{}/t{}/e{}/p{}", nonzero, fetch.size_2d.width + 1,
                              fetch.size_2d.height + 1, uint32_t(fetch.tiled),
                              uint32_t(fetch.endianness), uint32_t(fetch.pitch));
      // Raw guest bytes (first 256 KB) for offline decoding.
      if (const char* dir = std::getenv("MANTRA_CAPTURE_DIR"); dir && *dir) {
        const std::filesystem::path path =
            std::filesystem::path(dir) /
            fmt::format("tex_{:08X}_f{}.bin", fetch.base_address << 12, uint32_t(fetch.format));
        if (!std::filesystem::exists(path)) {
          if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
            std::fwrite(t, 1, 256 * 1024, file);
            std::fclose(file);
          }
        }
      }
    }
  }
  MANTRA_FRAME_TRACE(
      "{} mode {} prim {} n {} vs {:016X}{} ps {:016X}{} rt base {} fmt {} pitch {} "
      "mask {:X} blend {}/{}/{} {}/{}/{} z {}{} at {}{}",
      outcome, uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode), uint32_t(prim_type),
      index_count, vs ? vs->ucode_data_hash() : 0, vs && vs->module ? "" : "(none)",
      ps ? ps->ucode_data_hash() : 0, ps && ps->module ? "" : "(none)",
      uint32_t(color_info.color_base), uint32_t(color_info.color_format),
      uint32_t(surface_info.surface_pitch), regs.Get<reg::RB_COLOR_MASK>().value & 0xF,
      uint32_t(blend.color_srcblend), uint32_t(blend.color_destblend),
      uint32_t(blend.color_comb_fcn), uint32_t(blend.alpha_srcblend),
      uint32_t(blend.alpha_destblend), uint32_t(blend.alpha_comb_fcn), uint32_t(depth.z_enable),
      uint32_t(depth.zfunc), color_control.alpha_test_enable ? uint32_t(color_control.alpha_func) : 99,
      textures);
  MANTRA_FRAME_TRACE("  surf clip {:08X} mode {:08X} depthctl {:08X} sref_bf {:08X}",
              regs[XE_GPU_REG_PA_CL_CLIP_CNTL], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
              regs[XE_GPU_REG_RB_DEPTHCONTROL], regs[XE_GPU_REG_RB_STENCILREFMASK_BF]);
  MANTRA_FRAME_TRACE("  surf msaa {} depth base {} fmt {} stencil {} zw {} sref {} swmask {:X} sop {}/{}/{}/{}",
              uint32_t(surface_info.msaa_samples),
              uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_base),
              uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_format), uint32_t(depth.stencil_enable),
              uint32_t(depth.z_write_enable), uint32_t(regs.Get<reg::RB_STENCILREFMASK>().stencilref),
              uint32_t(regs.Get<reg::RB_STENCILREFMASK>().stencilwritemask),
              uint32_t(depth.stencilfail), uint32_t(depth.stencilzpass), uint32_t(depth.stencilzfail),
              uint32_t(depth.stencilfunc));
  if (vs) {
    std::string inputs;
    for (const auto& in : vs->vertex_inputs) {
      inputs += fmt::format(" [loc {} usage {} {} vf{} stride {} off {} fmt {}{}{}{}", in.location,
                            uint32_t(in.usage), in.valid ? "ok" : "INVALID", in.fetch_constant,
                            in.stride_words, in.offset_words, uint32_t(in.format),
                            in.is_signed ? " s" : "", in.normalized ? " n" : "",
                            in.is_uint ? " u" : "");
      // Raw guest words of this element for the buffer's first 3 vertices.
      const auto vfetch = regs.GetVertexFetch(in.fetch_constant);
      const uint32_t base = (uint32_t(vfetch.address) << 2) & 0x1FFFFFFF;
      inputs += fmt::format(" @{:08X} e{}", base, uint32_t(vfetch.endian));
      const uint32_t words = 4;  // (may run into the next element)
      for (uint32_t v = 0; v < 4 && in.valid; ++v) {
        inputs += " v";
        for (uint32_t c = 0; c < words; ++c) {
          const uint32_t at = base + (v * in.stride_words + uint32_t(in.offset_words) + c) * 4;
          if (const auto* w = memory_->TranslatePhysical<const uint8_t*>(at)) {
            uint32_t raw;
            std::memcpy(&raw, w, 4);
            raw = xenos::GpuSwap(raw, vfetch.endian);
            float f;
            std::memcpy(&f, &raw, 4);
            inputs += fmt::format("{}{:08X}({:g})", c ? "," : "", raw, f);
          }
        }
      }
      inputs += "]";
    }
    MANTRA_FRAME_TRACE("  vs inputs{}", inputs);
    MANTRA_FRAME_TRACE("  vs {:016X} fetches{}", vs->cache_entry ? vs->cache_entry->exact_hash : 0,
                vs->vfetch_debug);
    std::string vs_constants;
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4;
      // c32..c247 only when non-zero (e.g. UE3's light direction in c241).
      if (i >= 32 && i < 248 && !regs[base] && !regs[base + 1] && !regs[base + 2] &&
          !regs[base + 3]) {
        continue;
      }
      vs_constants += fmt::format(" c{}=({:g} {:g} {:g} {:g})", i, regs.Get<float>(base),
                                  regs.Get<float>(base + 1), regs.Get<float>(base + 2),
                                  regs.Get<float>(base + 3));
    }
    MANTRA_FRAME_TRACE("  vsc{}", vs_constants);
    const auto tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
    const auto br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
    MANTRA_FRAME_TRACE("  scissor {},{} - {},{}", tl.tl_x, tl.tl_y, br.br_x, br.br_y);
  }
  // PS float constants c0..c31 (they follow the 256 VS registers).
  std::string constants;
  for (uint32_t i = 0; i < 32; ++i) {
    const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4;
    constants += fmt::format(" c{}=({:g} {:g} {:g} {:g})", i, regs.Get<float>(base),
                             regs.Get<float>(base + 1), regs.Get<float>(base + 2),
                             regs.Get<float>(base + 3));
  }
  MANTRA_FRAME_TRACE("  ps{}", constants);
}

void NativeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  EndFrameTiming();
  ScopedNs swap_timer{frame_timing_.swap_ns};
  ++swap_stats_.swaps;
  g_guest_swaps.fetch_add(1, std::memory_order_relaxed);
  // The traced frame (TracingThisFrame) is the one after swap trace_swap_ - 1:
  // its lines go to a file of their own (mantra::TraceLine).
  if (!trace_file_.empty() && swap_stats_.swaps >= trace_swap_) {
    mantra::EndTraceFile();
    MANTRA_INFO("trace: frame {} written to {}", trace_swap_, trace_file_.string());
    trace_file_.clear();
  }
  if (trace_swap_ && swap_stats_.swaps + 1 == trace_swap_ && trace_file_.empty()) {
    trace_file_ = mantra::BeginTraceFile(trace_swap_);
    trace_picture_.clear();
    if (!trace_file_.empty() && TraceCaptureEnabled()) {
      trace_picture_ = std::filesystem::path(trace_file_).replace_extension(".png");
    }
  }
  buffer_cache_.FrameEnded();
  ReloadShaderCacheIfChanged();
  // MANTRA_TRACE_TRIGGER=<file>: when the file appears, trace the next frame
  // (and delete the file, so it can be triggered again). Once the traced
  // frame ends, <file>.done holds its number (the launcher's toast and its
  // Trace button read it).
  static const char* trace_trigger = std::getenv("MANTRA_TRACE_TRIGGER");
  if (trace_from_trigger_ && swap_stats_.swaps == trace_swap_) {
    trace_from_trigger_ = false;
    if (std::FILE* file = std::fopen((std::string(trace_trigger) + ".done").c_str(), "w")) {
      std::fprintf(file, "%llu\n", static_cast<unsigned long long>(trace_swap_));
      std::fclose(file);
    }
  }
  if (trace_trigger && *trace_trigger && (swap_stats_.swaps & 15) == 0 &&
      (!trace_swap_ || trace_swap_ <= swap_stats_.swaps)) {
    std::error_code ec;
    if (std::filesystem::remove(trace_trigger, ec)) {
      trace_swap_ = swap_stats_.swaps + 2;
      trace_from_trigger_ = true;
      MANTRA_INFO("trace: triggered at swap {}, tracing swap {}", swap_stats_.swaps, trace_swap_);
      // Debug: MANTRA_DUMP_PHYSICAL=<file> also writes all 512 MB of guest
      // physical memory, for offline searches.
      if (const char* dump = std::getenv("MANTRA_DUMP_PHYSICAL"); dump && *dump) {
        if (const auto* mem = memory_->TranslatePhysical<const uint8_t*>(0)) {
          if (std::FILE* file = std::fopen(dump, "wb")) {
            std::fwrite(mem, 1, 0x20000000, file);
            std::fclose(file);
            MANTRA_INFO("trace: dumped guest physical memory to {}", dump);
          }
        }
      }
    }
  }
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
    // MANTRA_CAPTURE_DIR: dump the frame and the resolved surfaces every
    // MANTRA_CAPTURE_EVERY (default 150) swaps.
    static const char* capture_dir = std::getenv("MANTRA_CAPTURE_DIR");
    static const uint32_t capture_every = [] {
      const char* every = std::getenv("MANTRA_CAPTURE_EVERY");
      const int value = every ? std::atoi(every) : 0;
      return value > 0 ? uint32_t(value) : 150u;
    }();
    if (presented && capture_dir && *capture_dir &&
        (swap_stats_.swaps % capture_every == 0 ||
         (trace_swap_ && swap_stats_.swaps == trace_swap_))) {
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
    // Settings > Frame traces > Save the picture: the traced frame as shown,
    // next to its log.
    if (presented && !trace_picture_.empty() && swap_stats_.swaps == trace_swap_) {
      CaptureSurface(*surface, width, height, fetch.swizzle, {}, trace_picture_);
      trace_picture_.clear();
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

  // At the internal resolution: the presenter scales it to the window.
  const uint32_t output_width = mantra::HostSize(width, surface.image.scale8);
  const uint32_t output_height = mantra::HostSize(height, surface.image.scale8);
  return presenter->RefreshGuestOutput(
      output_width, output_height, display_width, display_height,
      [&](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vk_context =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        context.SetIs8bpc(is_8bpc);
        VkDescriptorPool descriptor_pool = ReserveDescriptorSet();
        VkCommandBuffer cmd = BeginNonDrawCommands();
        if (!descriptor_pool || !cmd) {
          return false;
        }
        mantra::TransitionImage(vulkan_device_, cmd, surface.image,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        mantra::TrackedImage output = GuestOutputImage(vk_context, output_width, output_height);
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
            ui::vulkan::VulkanPresenter::kGuestOutputFormat, width, height, rect, constants,
            surface.image.scale8);
        mantra::TransitionImage(vulkan_device_, cmd, output,
                                ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout,
                                ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask);
        GpuMarkHere(GpuMarkKind::kPresent);
        // Submit now: the presenter may paint the image as soon as this
        // returns, and its submission must come after ours.
        return SubmitCommands() && recorded;
      });
}

void NativeCommandProcessor::CaptureSurface(mantra::ResolvedSurface& surface, uint32_t width,
                                            uint32_t height, uint32_t swizzle,
                                            const std::string& name,
                                            const std::filesystem::path& path) {
  CaptureImage(surface.image, surface.image.format, width, height, swizzle, name, path);
}

namespace {

// 8-bit RGB PNG with stored (uncompressed) deflate blocks: no zlib here, and
// a 1280x720 frame is 2.7 MB either way.
bool WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height,
              const std::vector<uint8_t>& rgb) {
  static const auto crc_table = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
    return table;
  }();
  auto crc = [&](const uint8_t* data, size_t size, uint32_t c = 0xFFFFFFFFu) {
    for (size_t i = 0; i < size; ++i) c = crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c;
  };
  auto be32 = [](std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
  };
  // Raw scanlines, each with filter type 0.
  const size_t stride = size_t(width) * 3;
  std::vector<uint8_t> raw;
  raw.reserve((stride + 1) * height);
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + y * stride, rgb.begin() + (y + 1) * stride);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t at = 0; at < raw.size() || raw.empty();) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (last) break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  be32(z, b << 16 | a);
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    be32(png, uint32_t(data.size()));
    const size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    be32(png, crc(png.data() + start, png.size() - start) ^ 0xFFFFFFFFu);
  };
  std::vector<uint8_t> header;
  be32(header, width);
  be32(header, height);
  header.insert(header.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  chunk("IHDR", header);
  chunk("IDAT", z);
  chunk("IEND", {});
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (!file) return false;
  const bool ok = std::fwrite(png.data(), 1, png.size(), file) == png.size();
  return std::fclose(file) == 0 && ok;
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31;
  const uint32_t exponent = (h >> 10) & 0x1F;
  const uint32_t mantissa = h & 0x3FF;
  if (!exponent) {
    const float value = std::ldexp(float(mantissa), -24);
    return sign ? -value : value;
  }
  uint32_t bits = sign | ((exponent == 31 ? 255 : exponent + 112) << 23) | (mantissa << 13);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

}  // namespace

void NativeCommandProcessor::CaptureImage(mantra::TrackedImage& image, VkFormat format,
                                          uint32_t width, uint32_t height, uint32_t swizzle,
                                          const std::string& name,
                                          const std::filesystem::path& out_path) {
  // Captures hold the image's real pixels (the render scale included).
  width = mantra::HostSize(width, image.scale8);
  height = mantra::HostSize(height, image.scale8);
  uint32_t texel_size;
  switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: texel_size = 4; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM: texel_size = 8; break;  // SNORM: fixed16 stored /32
    case VK_FORMAT_S8_UINT: texel_size = 1; break;  // stencil plane of a depth image
    // Depth plane of a depth image (X8_D24: D24_UNORM_S8; D32: D32_SFLOAT_S8).
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT: texel_size = 4; break;
    default: return;
  }
  const bool stencil_plane = format == VK_FORMAT_S8_UINT;
  const bool depth_plane =
      format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D32_SFLOAT;
  if (image.samples != VK_SAMPLE_COUNT_1_BIT ||
      (!stencil_plane && !depth_plane && image.aspect != VK_IMAGE_ASPECT_COLOR_BIT)) {
    return;
  }
  width = std::min(width, image.width);
  height = std::min(height, image.height);
  if (!width || !height) {
    return;
  }
  const VkDeviceSize size = VkDeviceSize(width) * height * texel_size;
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
  VkCommandBuffer cmd = BeginNonDrawCommands();
  if (cmd) {
    mantra::TransitionImage(vulkan_device_, cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = stencil_plane ? VK_IMAGE_ASPECT_STENCIL_BIT
                                         : depth_plane ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                       : VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    dfn.vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                               &region);
    const uint64_t submission = submission_current_;
    SubmitCommands();
    AwaitSubmission(submission);
    void* mapping = nullptr;
    if (dfn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapping) == VK_SUCCESS) {
      VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = memory;
      range.size = VK_WHOLE_SIZE;
      dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
      const uint8_t* bytes = static_cast<const uint8_t*>(mapping);
      const char* capture_dir = std::getenv("MANTRA_CAPTURE_DIR");
      const std::filesystem::path path =
          !out_path.empty() ? out_path
                            : std::filesystem::path(capture_dir ? capture_dir : "") / (name + ".ppm");
      const bool png = path.extension() == ".png";
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      double sum[4] = {}, peak[4] = {};
      std::FILE* file = png ? nullptr : std::fopen(path.string().c_str(), "wb");
      if (png || file) {
        if (file) std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        std::vector<uint8_t> pixels;  // PNG: the whole picture
        if (png) pixels.reserve(size_t(width) * height * 3);
        std::vector<uint8_t> row(size_t(width) * 3);
        for (uint32_t y = 0; y < height; ++y) {
          for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* t = bytes + (size_t(y) * width + x) * texel_size;
            float f[4] = {0, 0, 0, 1};
            uint32_t v = 0;
            std::memcpy(&v, t, std::min<uint32_t>(texel_size, 4));
            switch (format) {
              case VK_FORMAT_S8_UINT:  // 0, 1, 2, 3 -> black, red, green, blue-ish
                f[0] = (v & 1) ? 1.0f : 0.0f;
                f[1] = (v & 2) ? 1.0f : 0.0f;
                f[2] = v > 3 ? 1.0f : 0.0f;
                break;
              case VK_FORMAT_X8_D24_UNORM_PACK32:  // depth, x50 so reversed-Z far range shows
                f[3] = float(v & 0xFFFFFF) / 16777215.0f;
                f[0] = f[1] = f[2] = f[3] * 50.0f;
                break;
              case VK_FORMAT_D32_SFLOAT:
                std::memcpy(&f[3], &v, 4);
                f[0] = f[1] = f[2] = f[3] * 50.0f;
                break;
              case VK_FORMAT_R8G8B8A8_UNORM:
                for (uint32_t i = 0; i < 4; ++i) f[i] = float((v >> (8 * i)) & 0xFF) / 255.0f;
                break;
              case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                for (uint32_t i = 0; i < 3; ++i) f[i] = float((v >> (10 * i)) & 0x3FF) / 1023.0f;
                f[3] = float(v >> 30) / 3.0f;
                break;
              case VK_FORMAT_R16G16_SFLOAT:
                f[0] = HalfToFloat(uint16_t(v));
                f[1] = HalfToFloat(uint16_t(v >> 16));
                f[2] = 0;
                break;
              case VK_FORMAT_R32_SFLOAT:
                std::memcpy(&f[0], &v, 4);
                f[1] = f[2] = f[0];
                break;
              case VK_FORMAT_R16G16B16A16_UNORM: {
                uint16_t n[4];
                std::memcpy(n, t, 8);
                for (uint32_t i = 0; i < 4; ++i) f[i] = float(n[i]) / 65535.0f;
                break;
              }
              case VK_FORMAT_R16G16B16A16_SNORM: {  // fixed16 render targets hold value/32
                int16_t n[4];
                std::memcpy(n, t, 8);
                for (uint32_t i = 0; i < 4; ++i) f[i] = std::max(float(n[i]) / 32767.0f, -1.0f) * 32.0f;
                break;
              }
              default: {  // R16G16B16A16_SFLOAT
                uint16_t h[4];
                std::memcpy(h, t, 8);
                for (uint32_t i = 0; i < 4; ++i) f[i] = HalfToFloat(h[i]);
                break;
              }
            }
            uint16_t comps[4];
            for (uint32_t i = 0; i < 4; ++i) {
              sum[i] += f[i];
              peak[i] = std::max(peak[i], double(f[i]));
              comps[i] = uint16_t(std::clamp(f[i], 0.0f, 1.0f) * 65535.0f + 0.5f);
            }
            for (uint32_t i = 0; i < 3; ++i) {
              row[x * 3 + i] = uint8_t(Swizzled(comps, (swizzle >> (3 * i)) & 7) >> 8);
            }
          }
          if (file) {
            std::fwrite(row.data(), 1, row.size(), file);
          } else {
            pixels.insert(pixels.end(), row.begin(), row.end());
          }
        }
        if (file) std::fclose(file);
        if (png && !WritePng(path, width, height, pixels)) {
          MANTRA_WARN("could not write {}", path.string());
        }
        const double n = double(width) * height;
        MANTRA_INFO("captured {} ({}x{} fmt {}) mean ({:.4g} {:.4g} {:.4g} {:.4g}) max ({:.4g} "
                    "{:.4g} {:.4g} {:.4g})",
                    path.filename().string(), width, height, uint32_t(format), sum[0] / n,
                    sum[1] / n, sum[2] / n, sum[3] / n, peak[0], peak[1], peak[2], peak[3]);
      }
      if (png) {
        // The trace's picture: no raw extras.
      } else if (format == VK_FORMAT_R32_SFLOAT) {
        std::filesystem::path raw = path;
        raw.replace_extension(".f32");
        if (std::FILE* f32 = std::fopen(raw.string().c_str(), "wb")) {
          std::fwrite(bytes, 4, size_t(width) * height, f32);
          std::fclose(f32);
        }
      } else if (format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        // Raw RGBA halves: the .ppm clamps HDR (7e3 scenes) to [0, 1].
        std::filesystem::path raw = path;
        raw.replace_extension(".rgba16f");
        if (std::FILE* f16 = std::fopen(raw.string().c_str(), "wb")) {
          std::fwrite(bytes, 8, size_t(width) * height, f16);
          std::fclose(f16);
        }
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
        VkCommandBuffer cmd = BeginNonDrawCommands();
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

void NativeCommandProcessor::EndFrameTiming() {
  FrameTiming& t = frame_timing_;
  const auto now = std::chrono::steady_clock::now();
  const auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
  const uint64_t pipelines = draw_pipelines_.created() + draw_pipelines_.async_created();
  const uint64_t modules = shader_stats_.modules;
  const uint64_t texture_bytes = texture_cache_.stats().load_bytes;
  const auto& tcs = texture_cache_.stats();
  const uint64_t texture_ns[5] = {tcs.load_ns, tcs.watch_ns, tcs.hash_ns, tcs.swap_ns,
                                  tcs.untile_ns};
  const uint64_t buffer_bytes = buffer_cache_.stats().upload_bytes;
  const uint64_t vertex_bytes = guest_draw_stats_.vertex_bytes;
  const uint64_t expand_ns = guest_draw_stats_.expand_ns;
  const uint64_t thread_cpu_ns = ThreadCpuNs();
  ThreadTimes threads;
  if (ThreadProfileEveryFrame()) threads = ThreadTimes::Sample();
  if (t.last_swap.time_since_epoch().count()) {
    const double total = std::chrono::duration<double, std::milli>(now - t.last_swap).count();
    const double busy = ms(thread_cpu_ns - t.thread_cpu_ns);
    t.busy_ms += busy;
    ++t.frames;
    t.total_ms += total;
    t.max_ms = std::max(t.max_ms, total);
    t.over_50 += total > 50.0;
    t.over_100 += total > 100.0;
    t.fence_ms += ms(t.fence_ns);
    t.draw_ms += ms(t.draw_ns);
    t.copy_ms += ms(t.copy_ns);
    t.swap_ms += ms(t.swap_ns);
    // A hitch: log what this frame did (traced/captured frames are slow anyway).
    // "other": time outside draws/resolves/swap; "gpu thread busy" is the GPU
    // thread's CPU time over the whole frame, so other minus the busy part not
    // in draws etc. is roughly waiting for the game to submit.
    if (total > 70.0 && !TracingThisFrame()) {
      MANTRA_INFO("hitch: swap {} took {:.1f} ms (fence waits {:.1f}, draws {:.1f}, resolves "
                  "{:.1f}, swap {:.1f}, other {:.1f}; gpu thread busy {:.1f}); new pipelines {}, "
                  "shader modules {}, texture loads {:.1f} MB in {:.1f} ms (watch {:.1f}, hash "
                  "{:.1f}, swap {:.1f}, untile {:.1f}), buffer uploads {:.1f} MB, "
                  "vertex data {:.1f} MB (decoding {:.1f} ms); draw phases: textures {:.1f}, geometry "
                  "{:.1f}, constants {:.1f}, targets {:.1f}, pipelines {:.1f}, memexport {:.1f}, "
                  "fills {:.1f}, rest {:.1f}{}{}",
                  swap_stats_.swaps, total, ms(t.fence_ns), ms(t.draw_ns), ms(t.copy_ns),
                  ms(t.swap_ns), total - ms(t.draw_ns) - ms(t.copy_ns) - ms(t.swap_ns), busy,
                  pipelines - t.pipelines, modules - t.modules,
                  (texture_bytes - t.texture_bytes) / 1048576.0,
                  ms(texture_ns[0] - t.texture_ns[0]), ms(texture_ns[1] - t.texture_ns[1]),
                  ms(texture_ns[2] - t.texture_ns[2]), ms(texture_ns[3] - t.texture_ns[3]),
                  ms(texture_ns[4] - t.texture_ns[4]),
                  (buffer_bytes - t.buffer_bytes) / 1048576.0,
                  (vertex_bytes - t.vertex_bytes) / 1048576.0, ms(expand_ns - t.expand_ns),
                  ms(t.phase_ns[0]), ms(t.phase_ns[1]), ms(t.phase_ns[2]), ms(t.phase_ns[3]),
                  ms(t.phase_ns[4]), ms(t.phase_ns[5]), ms(t.phase_ns[6]),
                  // Resolves run inside draws (copy draws).
                  ms(t.draw_ns) - ms(t.copy_ns) -
                      ms(std::accumulate(std::begin(t.phase_ns), std::end(t.phase_ns), uint64_t(0))),
                  ThreadProfileEveryFrame() ? "; busiest threads: " : "",
                  ThreadProfileEveryFrame() ? threads.Busiest(g_frame_threads, total, 5) : "");
    }
  }
  t.last_swap = now;
  t.fence_ns = t.draw_ns = t.copy_ns = t.swap_ns = 0;
  std::fill(std::begin(t.phase_ns), std::end(t.phase_ns), uint64_t(0));
  t.pipelines = pipelines;
  t.modules = modules;
  t.texture_bytes = texture_bytes;
  std::copy(std::begin(texture_ns), std::end(texture_ns), std::begin(t.texture_ns));
  t.buffer_bytes = buffer_bytes;
  t.vertex_bytes = vertex_bytes;
  t.expand_ns = expand_ns;
  t.thread_cpu_ns = thread_cpu_ns;
  if (ThreadProfileEveryFrame()) g_frame_threads = std::move(threads);
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
  MANTRA_DEBUG(
      "swaps: {} total ({:.1f}/s), presented {}, skipped {}, refresh failed {}; last frame "
      "{:.1f}% non-black",
      swap_stats_.swaps, swaps_per_second, swap_stats_.presented, swap_stats_.skipped,
      swap_stats_.refresh_failed, swap_stats_.last_lit_permille / 10.0);
  {
    FrameTiming& t = frame_timing_;
    const double n = std::max<double>(double(t.frames), 1.0);
    MANTRA_INFO("frame times: avg {:.1f} ms, max {:.1f} ms, {} over 50 ms, {} over 100 ms; per "
                "frame: fence waits {:.1f} ms, draws {:.1f} ms, resolves {:.1f} ms, swap {:.1f} "
                "ms, other {:.1f} ms",
                t.total_ms / n, t.max_ms, t.over_50, t.over_100, t.fence_ms / n, t.draw_ms / n,
                t.copy_ms / n, t.swap_ms / n,
                (t.total_ms - t.draw_ms - t.copy_ms - t.swap_ms) / n);
    // Where the CPU went: the GPU thread's share, then the busiest threads.
    const ThreadTimes threads = ThreadTimes::Sample();
    if (g_window_threads_time.time_since_epoch().count()) {
      const double wall = std::chrono::duration<double, std::milli>(now - g_window_threads_time).count();
      MANTRA_DEBUG("cpu: gpu thread busy {:.1f} ms per frame ({:.0f}%); busiest threads: {}",
                  t.busy_ms / n, 100.0 * t.busy_ms / std::max(t.total_ms, 1e-3),
                  threads.Busiest(g_window_threads, wall, 6));
    }
    g_window_threads = threads;
    g_window_threads_time = now;
    ReportGpuProfile(double(t.frames));
    draw_pipelines_.SaveCacheIfChanged();
    t.frames = t.over_50 = t.over_100 = 0;
    t.max_ms = t.total_ms = t.fence_ms = t.draw_ms = t.copy_ms = t.swap_ms = t.busy_ms = 0;
  }
  MANTRA_DEBUG("  presented {} from resolves, {} from guest memory; draws {}, resolves {} "
              "(failed {}, color copies {}, depth copies {}, skipped {}, unsupported dest {}), "
              "clears color {} depth {}; 7e3 conversions {}; solid fills color {} depth {} skipped {}; {} render targets, "
              "{} resolved surfaces",
              swap_stats_.presented_gpu, swap_stats_.presented_cpu, draw_stats_.draws,
              draw_stats_.copy_draws, draw_stats_.resolves_failed, draw_stats_.color_copies,
              draw_stats_.depth_copies, draw_stats_.depth_copies_skipped, draw_stats_.unsupported_dest_format,
              draw_stats_.color_clears, draw_stats_.depth_clears, draw_stats_.float7e3_conversions,
              draw_stats_.fills_color,
              draw_stats_.fills_depth, draw_stats_.fills_skipped, render_targets_.size(),
              resolved_surfaces_.size());
  MANTRA_DEBUG("shaders: {} loads, {} unique, {} dumped; cache {} exact, {} normalized "
              "({} remapped), {} failed, {} miss; {} modules ({} failed)",
              shader_stats_.loads, shader_stats_.unique, shader_stats_.dumped,
              shader_stats_.cache_exact, shader_stats_.cache_normalized, shader_stats_.remapped,
              shader_stats_.cache_failed, shader_stats_.cache_miss, shader_stats_.modules,
              shader_stats_.module_failures);
  MANTRA_DEBUG("constants: {} draws prepared, {} without translation; {} uploads ({:.1f} MB), "
              "{} reused, {} ring-full submits; last submission used {} KB of the ring",
              constant_stats_.draws, constant_stats_.no_translation, constant_stats_.uploads,
              constant_stats_.bytes / 1048576.0, constant_stats_.reused,
              constant_stats_.ring_full, shader_interface_.slot_bytes_used() >> 10);
  const GuestDrawStats& g = guest_draw_stats_;
  MANTRA_DEBUG("pipelines: {} in use, {} created in game ({} in the background, {} draws skipped "
              "while building; {} failed); warm-up {}/{} built, {} used",
              draw_pipelines_.size(), draw_pipelines_.created() + draw_pipelines_.async_created(),
              draw_pipelines_.async_created(), g.skipped_pending_pipeline,
              draw_pipelines_.creation_failures(), draw_pipelines_.warm_done(),
              draw_pipelines_.warm_total(), draw_pipelines_.warm_hits());
  MANTRA_DEBUG("guest draws: {} drawn in {} render passes, {} pipelines ({} failed); skipped: "
              "primitive {}, no target {}, viewport {}, geometry {}; {} zero-fed inputs, {} expanded "
              "(decoding {:.0f} ms); streamed {:.1f} MB vertices, {:.1f} MB indices",
              g.drawn, g.render_passes, draw_pipelines_.size(), draw_pipelines_.creation_failures(),
              g.skipped_primitive, g.skipped_no_target, g.skipped_viewport, g.skipped_geometry,
              g.missing_inputs, g.expanded_inputs, g.expand_ns / 1e6, g.vertex_bytes / 1048576.0, g.index_bytes / 1048576.0);
  MANTRA_DEBUG("memexport: {} draws, {} vertices, {} elements ({:.1f} MB), {} unsupported",
              memexport_stats_.draws, memexport_stats_.vertices, memexport_stats_.elements,
              memexport_stats_.bytes / 1048576.0, memexport_stats_.unsupported);
  const auto& bc = buffer_cache_.stats();
  MANTRA_DEBUG("buffer cache: {} entries in {} blocks; {} hits, {} uploads ({} re-uploads, "
              "{:.1f} MB), {} CPU-write invalidations, {} uncacheable, {} volatile, {} resets",
              buffer_cache_.size(), buffer_cache_.blocks_in_use(), bc.hits, bc.uploads,
              bc.reuploads, bc.upload_bytes / 1048576.0, buffer_cache_.invalidations(),
              bc.not_cacheable, bc.volatile_skips, bc.resets);
  const auto& tc = texture_cache_.stats();
  MANTRA_DEBUG("textures: {} cached, {} samplers; {} loads ({} after CPU writes, {:.1f} MB), "
              "{} from resolves ({} sampled directly, {} copies, {} not copyable), {} unsupported, {} failed; {} volatile "
              "({} hashes); descriptor sets {} ({} reused); loads took {:.0f} ms (watch {:.0f}, "
              "hash {:.0f}, swap {:.0f}, untile {:.0f})",
              texture_cache_.size(), texture_cache_.sampler_count(), tc.loads, tc.reloads,
              tc.load_bytes / 1048576.0, tc.resolved_binds, tc.resolved_direct, tc.resolved_copies,
              tc.resolve_mismatches, tc.unsupported, tc.failures,
              tc.volatile_marks, tc.volatile_hashes, g.texture_sets, g.texture_sets_reused,
              tc.load_ns / 1e6, tc.watch_ns / 1e6, tc.hash_ns / 1e6, tc.swap_ns / 1e6,
              tc.untile_ns / 1e6);
  swap_stats_.last_report_time = now;
  swap_stats_.swaps_at_last_report = swap_stats_.swaps;
  // The draw census every 30 seconds.
  if (++swap_stats_.reports % 6 == 0) {
    ReportDrawCensus();
  }
}

namespace {

// Renumbers the Location decorations of a SPIR-V module's Output variables
// below 16 through `map`. Returns the number of decorations changed.
uint32_t RemapOutputLocations(std::vector<uint8_t>& module, const uint8_t (&map)[16]) {
  const size_t words = module.size() / 4;
  auto* code = reinterpret_cast<uint32_t*>(module.data());
  constexpr uint32_t kOpDecorate = 71, kOpVariable = 59, kDecorationLocation = 30,
                     kStorageClassOutput = 3;
  std::unordered_set<uint32_t> outputs;
  for (size_t at = 5; at < words;) {
    const uint32_t count = code[at] >> 16, opcode = code[at] & 0xFFFF;
    if (!count || at + count > words) break;
    if (opcode == kOpVariable && count >= 4 && code[at + 3] == kStorageClassOutput) {
      outputs.insert(code[at + 2]);
    }
    at += count;
  }
  uint32_t changed = 0;
  for (size_t at = 5; at < words;) {
    const uint32_t count = code[at] >> 16, opcode = code[at] & 0xFFFF;
    if (!count || at + count > words) break;
    if (opcode == kOpDecorate && count == 4 && code[at + 2] == kDecorationLocation &&
        outputs.count(code[at + 1]) && code[at + 3] < 16) {
      if (map[code[at + 3]] != code[at + 3]) ++changed;
      code[at + 3] = map[code[at + 3]];
    }
    at += count;
  }
  return changed;
}

}  // namespace

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
  mantra::ShaderCache::Lookup lookup;
  {
    ++shader_stats_.unique;
    MANTRA_DEBUG("new {} shader {:016X}: {} dwords", is_pixel ? "pixel" : "vertex", hash,
                 dword_count);
    DumpShader(hash, is_pixel, host_address, byte_count);
    lookup = shader_cache_.Find(host_address, byte_count, is_pixel);
    if (lookup.match != mantra::ShaderCache::Match::kExact) {
      KeepForNextCacheBuild(hash, is_pixel, host_address, byte_count);
    }
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
  auto shader = std::make_unique<MantraShader>(shader_type, hash, host_address, dword_count);
  shader->AnalyzeUcode(ucode_disasm_buffer_);
  AttachTranslation(*shader, lookup, host_address, byte_count);
  if (lookup.match != mantra::ShaderCache::Match::kExact) {
    shader->ucode_be.assign(host_address, host_address + dword_count);
  }
  Shader* result = shader.get();
  shaders.emplace(hash, std::move(shader));
  return result;
}

bool NativeCommandProcessor::AttachTranslation(MantraShader& shader,
                                               const mantra::ShaderCache::Lookup& lookup,
                                               const uint32_t* microcode_be, size_t byte_count) {
  if (lookup.spirv.empty()) {
    return false;
  }
  const bool is_pixel = shader.type() == xenos::ShaderType::kPixel;
  shader.cache_entry = *lookup.entry;
  shader.ps_outputs = lookup.entry->ps_outputs;
  shader.vertex_index_divisor = lookup.entry->vertex_index_divisor;
  if (!is_pixel) {
    shader.DecodeVertexInputs(lookup.vertex_elements, lookup.vertex_swizzles,
                              lookup.container_formats);
  }
  std::span<const uint8_t> spirv = lookup.spirv;
  std::vector<uint8_t> remapped;
  if (!is_pixel && lookup.match == mantra::ShaderCache::Match::kNormalized &&
      !lookup.exports.empty()) {
    // The driver patched this shader's exports for the pixel shader it is
    // drawn with: renumber the translation's interpolator outputs to match.
    uint8_t map[16];
    const std::vector<uint32_t> runtime_exports =
        mantra::shader_key::InterpolatorExports(microcode_be, byte_count);
    if (!mantra::shader_key::InterpolatorLocationMap(lookup.exports, runtime_exports, map)) {
      // Identity, or exports that don't correspond (then left as is).
      auto list = [](const std::vector<uint32_t>& exports) {
        std::string s;
        for (uint32_t e : exports) {
          s += fmt::format(" {}:{}{}", e >> 8, (e >> 4) & 1 ? 's' : 'v', e & 0xF);
        }
        return s;
      };
      if (lookup.uncovered) {
        MANTRA_WARN("vs {:016X} (cached {:016X}, {} candidates): runtime exports not all "
                    "translated; translated{} | runtime{}",
                    shader.ucode_data_hash(), lookup.entry->exact_hash, lookup.candidates, list(lookup.exports),
                    list(runtime_exports));
      }
    } else {
      remapped.assign(spirv.begin(), spirv.end());
      const uint32_t patched = RemapOutputLocations(remapped, map);
      spirv = remapped;
      ++shader_stats_.remapped;
      std::string text;
      for (uint32_t i = 0; i < 16; ++i) {
        if (map[i] != i) text += fmt::format(" o{}->{}", i, map[i]);
      }
      MANTRA_DEBUG("vs {:016X} (cached {:016X}, {} candidates{}): interpolators{} ({} "
                  "decorations)",
                  shader.ucode_data_hash(), lookup.entry->exact_hash, lookup.candidates,
                  lookup.uncovered ? ", exports not all translated" : "", text, patched);
    }
  }
  shader.module = shader_interface_.CreateModule(spirv);
  if (shader.module) {
    ++shader_stats_.modules;
  } else {
    ++shader_stats_.module_failures;
    MANTRA_WARN("shader {:016X}: vkCreateShaderModule failed", shader.ucode_data_hash());
  }
  return shader.module != VK_NULL_HANDLE;
}

void NativeCommandProcessor::ReloadShaderCacheIfChanged() {
  if (shader_cache_loading_.valid()) {
    if (shader_cache_loading_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      return;
    }
    PendingShaderCache loaded = shader_cache_loading_.get();
    if (!loaded.cache) {
      return;  // being written (it is renamed into place), or not a cache: next time
    }
    shader_cache_time_ = loaded.time;
    shader_cache_bytes_ = loaded.bytes;
    shader_cache_ = std::move(*loaded.cache);
  } else {
    const auto now = std::chrono::steady_clock::now();
    if (shader_cache_path_.empty() || now - shader_cache_checked_ < std::chrono::seconds(2)) {
      return;
    }
    shader_cache_checked_ = now;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(shader_cache_path_, ec);
    if (ec) return;
    const uintmax_t bytes = std::filesystem::file_size(shader_cache_path_, ec);
    if (ec || (time == shader_cache_time_ && bytes == shader_cache_bytes_)) return;
    shader_cache_loading_ = std::async(std::launch::async, [path = shader_cache_path_, time, bytes] {
      PendingShaderCache loaded;
      auto cache = std::make_unique<mantra::ShaderCache>();
      if (cache->Load(path)) {
        loaded.cache = std::move(cache);
        loaded.time = time;
        loaded.bytes = bytes;
      }
      return loaded;
    });
    return;
  }
  // Shaders without an exact translation: look them up again.
  uint32_t added = 0, improved = 0, waiting = 0;
  for (auto& shaders : shaders_) {
    for (auto& [hash, base] : shaders) {
      auto& shader = *static_cast<MantraShader*>(base.get());
      if (shader.ucode_be.empty()) continue;
      const size_t byte_count = shader.ucode_be.size() * sizeof(uint32_t);
      const bool is_pixel = shader.type() == xenos::ShaderType::kPixel;
      mantra::ShaderCache::Lookup lookup =
          shader_cache_.Find(shader.ucode_be.data(), byte_count, is_pixel);
      const bool had = shader.module != VK_NULL_HANDLE;
      // Better than before: a translation where there was none, or an exact
      // one for a normalized match.
      if (lookup.spirv.empty() ||
          (had && lookup.match != mantra::ShaderCache::Match::kExact)) {
        waiting += !had;
        continue;
      }
      const VkShaderModule old = shader.module;
      shader.module = VK_NULL_HANDLE;
      shader.vertex_inputs.clear();
      if (!AttachTranslation(shader, lookup, shader.ucode_be.data(), byte_count)) {
        shader.module = old;
        continue;
      }
      if (old) retired_modules_.push_back(old);
      (had ? improved : added)++;
      if (lookup.match == mantra::ShaderCache::Match::kExact) {
        shader.ucode_be.clear();
        shader.ucode_be.shrink_to_fit();
      }
    }
  }
  MANTRA_INFO("shader cache rebuilt: {} entries; {} shaders now translated, {} made exact, {} "
              "still missing", shader_cache_.size(), added, improved, waiting);
}

void NativeCommandProcessor::ReportDrawCensus() {
  std::vector<std::pair<uint64_t, std::tuple<uint64_t, uint64_t, uint32_t>>> entries;
  for (const auto& [key, count] : draw_census_) entries.emplace_back(count, key);
  std::sort(entries.begin(), entries.end(), std::greater<>());
  auto dwords = [&](uint32_t type, uint64_t hash) -> size_t {
    auto it = shaders_[type].find(hash);
    return it != shaders_[type].end() ? it->second->ucode_dword_count() : 0;
  };
  MANTRA_DEBUG("draw census: {} distinct (vs, ps, edram mode, prim)", entries.size());
  for (size_t i = 0; i < std::min<size_t>(entries.size(), 16); ++i) {
    const auto& [count, key] = entries[i];
    const auto& [vs, ps, mode] = key;
    MANTRA_DEBUG("  {:7} x vs {:016X} ({} dw) ps {:016X} ({} dw) edram {} prim {}", count, vs,
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
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (!file) {
    MANTRA_WARN_ONCE("cannot write shader dumps to {}", dump_dir);
    return;
  }
  std::fwrite(microcode, 1, byte_count, file);
  std::fclose(file);
  ++shader_stats_.dumped;
}

void NativeCommandProcessor::KeepForNextCacheBuild(uint64_t hash, bool is_pixel,
                                                   const uint32_t* microcode, size_t byte_count) {
  // Programs the D3D runtime patched at draw time (vertex formats, export
  // swizzles) only exist here, never in the game files: a cache built from
  // the files alone has only their unpatched originals (normalized matches,
  // which can draw wrong or not at all, e.g. the menu text). Kept next to the
  // cache, where the launcher's shader build reads them (mantra_shadergen
  // --dumps) to translate them exactly.
  static const std::filesystem::path dir = [] {
    const auto cache = mantra::ShaderCache::DefaultPath();
    return cache.empty() ? cache : cache.parent_path() / "shader_dumps";
  }();
  if (dir.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path path =
      dir / fmt::format("{}_{:016X}_{}.mc", is_pixel ? "ps" : "vs", hash, byte_count / 4);
  if (std::filesystem::exists(path, ec)) {
    return;
  }
  // Written under a temporary name, so a build never reads half a file.
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  if (std::FILE* file = std::fopen(temporary.string().c_str(), "wb")) {
    const bool ok = std::fwrite(microcode, 1, byte_count, file) == byte_count;
    std::fclose(file);
    if (ok) {
      std::filesystem::rename(temporary, path, ec);
      MANTRA_DEBUG("shader {:016X} is not in the cache exactly; kept for the next cache build", hash);
      return;
    }
  }
  std::filesystem::remove(temporary, ec);
  MANTRA_WARN_ONCE("cannot keep shaders for the next cache build in {}", dir.string());
}

// --- Guest shader constants ---------------------------------------------------

bool NativeCommandProcessor::UploadConstants(const void* data, uint32_t size, uint32_t& offset) {
  if (!BeginCommands()) {
    return false;
  }
  if (shader_interface_.Upload(data, size, offset)) {
    return true;
  }
  // This submission's ring region is full: submit and continue in the next.
  ++constant_stats_.ring_full;
  SubmitCommands();
  return BeginCommands() && shader_interface_.Upload(data, size, offset);
}

bool NativeCommandProcessor::PrepareDrawConstants(mantra::DrawConstantOffsets& offsets,
                                                  const mantra::SharedConstants& draw_shared) {
  auto* vs = static_cast<MantraShader*>(active_vertex_shader());
  auto* ps = static_cast<MantraShader*>(active_pixel_shader());
  if (!vs || !vs->module || (ps && !ps->module)) {
    ++constant_stats_.no_translation;
    return false;
  }
  ++constant_stats_.draws;
  const RegisterFile& regs = *register_file_;

  // Float constants: D3D on the 360 splits the 512 registers evenly, VS
  // c0..c255 then PS c0..c255 (the translated shaders index from 0).
  const auto* float_regs =
      reinterpret_cast<const uint8_t*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);

  mantra::SharedConstants shared = draw_shared;
  // Bool constants: the translated shaders test VS b0..15 (guest bool
  // registers 0..15) as bits 0..15 and PS b0..15 (registers 128..143) as bits
  // 16..31.
  shared.booleans = (regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031] & 0xFFFF) |
                    (regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_128_159] << 16);
  shared.alpha_threshold = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);

  auto stage = [&](ConstantCache& cache, const void* data, uint32_t size,
                   uint32_t& offset) -> bool {
    // Submissions may change inside UploadConstants; compare against the
    // submission being recorded.
    if (cache.submission == submission_current_ && recording_ && cache.data.size() == size &&
        std::memcmp(cache.data.data(), data, size) == 0) {
      offset = cache.offset;
      ++constant_stats_.reused;
      return true;
    }
    if (!UploadConstants(data, size, offset)) {
      return false;
    }
    cache.submission = submission_current_;
    cache.offset = offset;
    cache.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    ++constant_stats_.uploads;
    constant_stats_.bytes += size;
    return true;
  };
  constexpr uint32_t kFloatBytes = mantra::ShaderInterface::kFloatConstantBytes;
  if (!stage(vs_constants_, float_regs, kFloatBytes, offsets.vertex)) return false;
  if (!stage(ps_constants_, float_regs + kFloatBytes, kFloatBytes, offsets.pixel)) return false;
  if (!stage(shared_constants_, &shared, sizeof(shared), offsets.shared)) return false;
  // A ring-full submit between stages invalidates the earlier offsets of
  // this draw; redo them in the new submission.
  if (vs_constants_.submission != submission_current_ ||
      ps_constants_.submission != submission_current_) {
    return stage(vs_constants_, float_regs, kFloatBytes, offsets.vertex) &&
           stage(ps_constants_, float_regs + kFloatBytes, kFloatBytes, offsets.pixel);
  }
  return true;
}

void NativeCommandProcessor::DestroyShaderModules() {
  const auto& dfn = vulkan_device_->functions();
  for (VkShaderModule module : retired_modules_) {
    dfn.vkDestroyShaderModule(vulkan_device_->device(), module, nullptr);
  }
  retired_modules_.clear();
  for (auto& shaders : shaders_) {
    for (auto& [hash, shader] : shaders) {
      auto* s = static_cast<MantraShader*>(shader.get());
      if (s->module) {
        dfn.vkDestroyShaderModule(vulkan_device_->device(), s->module, nullptr);
        s->module = VK_NULL_HANDLE;
      }
    }
  }
}

}  // namespace rex::graphics::native
