#include "draw_pipelines.h"
#include "mantra_log.h"
#include "../platform/platform.h"


#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include <xxhash.h>

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>

namespace mantra {

namespace {

// SPIR-V compiled at build time by glslc (see CMakeLists.txt).
const uint32_t kRectListGs[] = {
#include "rect_list.geom.inc"
};

}  // namespace

size_t DrawPipelines::KeyHash::operator()(const PipelineKey& key) const {
  return size_t(XXH3_64bits(&key, sizeof(key)));
}

bool DrawPipelines::Initialize(const rex::ui::vulkan::VulkanDevice* device,
                               VkPipelineLayout layout, RenderingBackend* rendering) {
  device_ = device;
  rendering_ = rendering;
  layout_ = layout;
  if (device->properties().geometryShader) {
    rect_list_gs_ =
        rex::ui::vulkan::util::CreateShaderModule(device, kRectListGs, sizeof(kRectListGs));
  }
  if (!rect_list_gs_) {
    MANTRA_WARN("no geometry shaders: rectangle-list draws will be skipped");
  }
  const char* env = std::getenv("MANTRA_ASYNC_PIPELINES");
  async_enabled_ = !(env && *env == '0');
  if (async_enabled_) {
    for (int i = 0; i < 2; ++i) async_threads_.emplace_back([this] { AsyncWorker(); });
  }
  return true;
}

void DrawPipelines::Shutdown() {
  if (!device_) {
    return;
  }
  StopWarmUp();
  {
    std::lock_guard<std::mutex> lock(warm_mutex_);
    async_stop_ = true;
    async_queue_.clear();
  }
  async_cv_.notify_all();
  for (std::thread& t : async_threads_) {
    if (t.joinable()) t.join();
  }
  async_threads_.clear();
  if (save_thread_.joinable()) save_thread_.join();
  if (cache_ && cache_changes_.load() != cache_saved_changes_) {
    SaveCache();
  }
  const auto& dfn = device_->functions();
  const VkDevice device = device_->device();
  for (auto& [key, pipeline] : pipelines_) {
    if (pipeline) dfn.vkDestroyPipeline(device, pipeline, nullptr);
  }
  pipelines_.clear();
  for (auto& [key, pipeline] : warm_ready_) {
    if (pipeline) dfn.vkDestroyPipeline(device, pipeline, nullptr);
  }
  warm_ready_.clear();
  warm_keys_.clear();
  if (cache_) {
    destroy_cache_(device, cache_, nullptr);
    cache_ = VK_NULL_HANDLE;
  }
  if (rect_list_gs_) {
    dfn.vkDestroyShaderModule(device, rect_list_gs_, nullptr);
    rect_list_gs_ = VK_NULL_HANDLE;
  }
  device_ = nullptr;
}

VkPipeline DrawPipelines::Get(const PipelineKey& key, bool* pending) {
  if (auto it = pipelines_.find(key); it != pipelines_.end()) {
    return it->second;
  }
  const bool async = pending && async_enabled_;
  if (pending) *pending = false;
  VkPipeline pipeline = VK_NULL_HANDLE;
  bool found = false;
  if (!warm_keys_.empty() || async) {
    std::lock_guard<std::mutex> lock(warm_mutex_);
    if (auto it = warm_ready_.find(key); it != warm_ready_.end()) {
      pipeline = it->second;
      warm_ready_.erase(it);
      found = true;
      if (async_pending_.erase(key)) {
        ++async_created_;
        if (!pipeline) ++creation_failures_;
      } else {
        ++warm_hits_;
      }
    } else if (async) {
      // Not built yet: queue it once (if the warm-up is building the same
      // key, the second build is dropped when it lands) and skip the draw.
      if (async_pending_.emplace(key, 0).second) {
        async_queue_.push_back(key);
        async_cv_.notify_one();
      }
      ++pending_skips_;
      *pending = true;
      return VK_NULL_HANDLE;
    }
  }
  if (!found) {
    pipeline = Create(key);
    ++created_;
    if (!pipeline) {
      ++creation_failures_;
    }
  }
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

void DrawPipelines::AsyncWorker() {
  for (;;) {
    PipelineKey key;
    {
      std::unique_lock<std::mutex> lock(warm_mutex_);
      async_cv_.wait(lock, [this] { return async_stop_ || !async_queue_.empty(); });
      if (async_stop_) return;
      key = async_queue_.front();
      async_queue_.pop_front();
      if (warm_ready_.count(key)) continue;  // warm-up built it meanwhile
    }
    const VkPipeline pipeline = Create(key);
    std::lock_guard<std::mutex> lock(warm_mutex_);
    // Failures land as VK_NULL_HANDLE so Get remembers them.
    if (!warm_ready_.emplace(key, pipeline).second && pipeline) {
      device_->functions().vkDestroyPipeline(device_->device(), pipeline, nullptr);
    }
  }
}

void DrawPipelines::OpenCache(const std::filesystem::path& path) {
  if (cache_ || !device_) {
    return;
  }
  const auto& ifn = device_->vulkan_instance()->functions();
  const VkDevice device = device_->device();
  auto create = reinterpret_cast<PFN_vkCreatePipelineCache>(
      ifn.vkGetDeviceProcAddr(device, "vkCreatePipelineCache"));
  destroy_cache_ = reinterpret_cast<PFN_vkDestroyPipelineCache>(
      ifn.vkGetDeviceProcAddr(device, "vkDestroyPipelineCache"));
  get_cache_data_ = reinterpret_cast<PFN_vkGetPipelineCacheData>(
      ifn.vkGetDeviceProcAddr(device, "vkGetPipelineCacheData"));
  if (!create || !destroy_cache_ || !get_cache_data_) {
    return;
  }
  std::vector<char> data;
  if (std::ifstream in(path, std::ios::binary); in) {
    data.assign(std::istreambuf_iterator<char>(in), {});
  }
  // The driver checks the header (vendor, device, cache UUID) and ignores
  // data it can't use.
  VkPipelineCacheCreateInfo info = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  info.initialDataSize = data.size();
  info.pInitialData = data.empty() ? nullptr : data.data();
  if (create(device, &info, nullptr, &cache_) != VK_SUCCESS) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    if (create(device, &info, nullptr, &cache_) != VK_SUCCESS) {
      cache_ = VK_NULL_HANDLE;
      return;
    }
  }
  cache_path_ = path;
  MANTRA_INFO("pipeline cache: {} ({:.1f} MB)", path.string(), data.size() / 1048576.0);
}

void DrawPipelines::SaveCache() {
  const VkDevice device = device_->device();
  const uint64_t changes = cache_changes_.load();
  size_t size = 0;
  if (get_cache_data_(device, cache_, &size, nullptr) != VK_SUCCESS || !size) {
    return;
  }
  std::vector<char> data(size);
  if (get_cache_data_(device, cache_, &size, data.data()) != VK_SUCCESS) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(cache_path_.parent_path(), ec);
  const std::filesystem::path temp = cache_path_.string() + ".tmp";
  if (std::FILE* file = std::fopen(temp.string().c_str(), "wb")) {
    const bool ok = std::fwrite(data.data(), 1, size, file) == size;
    if (std::fclose(file) == 0 && ok) {
      std::filesystem::rename(temp, cache_path_, ec);
      if (!ec) cache_saved_changes_ = changes;
    }
  }
}

void DrawPipelines::SaveCacheIfChanged() {
  if (!cache_ || cache_changes_.load() == cache_saved_changes_) {
    return;
  }
  if (save_thread_.joinable()) {
    save_thread_.join();
  }
  save_thread_ = std::thread([this] { SaveCache(); });
}

void DrawPipelines::WarmUp(std::vector<PipelineKey> keys) {
  StopWarmUp();
  if (keys.empty()) {
    return;
  }
  warm_keys_ = std::move(keys);
  warm_next_ = 0;
  warm_done_ = 0;
  warm_stop_ = false;
  warm_start_ = std::chrono::steady_clock::now();
  const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
  const unsigned threads = std::clamp(cores / 2, 1u, 4u);
  for (unsigned i = 0; i < threads; ++i) {
    warm_threads_.emplace_back([this] { WarmWorker(); });
  }
  MANTRA_INFO("pipeline warm-up: building {} pipelines from earlier runs on {} threads",
              warm_keys_.size(), threads);
}

void DrawPipelines::WarmWorker() {
  // Below the game's threads: this only has to finish before it's needed.
  asura::platform::LowerThreadPriority();
  for (;;) {
    const size_t i = warm_next_.fetch_add(1);
    if (i >= warm_keys_.size() || warm_stop_.load(std::memory_order_relaxed)) {
      break;
    }
    const PipelineKey& key = warm_keys_[i];
    const VkPipeline pipeline = Create(key);
    if (pipeline) {
      std::lock_guard<std::mutex> lock(warm_mutex_);
      if (!warm_ready_.emplace(key, pipeline).second) {
        device_->functions().vkDestroyPipeline(device_->device(), pipeline, nullptr);
      }
    }
    if (warm_done_.fetch_add(1) + 1 == warm_keys_.size()) {
      MANTRA_INFO("pipeline warm-up: {} pipelines built in {:.0f} ms", warm_keys_.size(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                            warm_start_)
                      .count());
    }
  }
}

void DrawPipelines::StopWarmUp() {
  warm_stop_ = true;
  for (std::thread& t : warm_threads_) {
    if (t.joinable()) t.join();
  }
  warm_threads_.clear();
}

VkPipeline DrawPipelines::Create(const PipelineKey& key) {
  const auto& dfn = device_->functions();

  VkSpecializationMapEntry spec_entry = {0, 0, sizeof(uint32_t)};
  VkSpecializationInfo spec_info = {1, &spec_entry, sizeof(key.spec_constants),
                                    &key.spec_constants};
  VkPipelineShaderStageCreateInfo stages[3] = {};
  uint32_t stage_count = 0;
  auto add_stage = [&](VkShaderStageFlagBits stage, uint64_t module, bool specialized) {
    VkPipelineShaderStageCreateInfo& s = stages[stage_count++];
    s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    s.stage = stage;
    s.module = VkShaderModule(module);
    s.pName = "main";
    s.pSpecializationInfo = specialized ? &spec_info : nullptr;
  };
  add_stage(VK_SHADER_STAGE_VERTEX_BIT, key.vertex_module, true);
  if (key.rect_list) {
    if (!rect_list_gs_) {
      return VK_NULL_HANDLE;
    }
    add_stage(VK_SHADER_STAGE_GEOMETRY_BIT, uint64_t(rect_list_gs_), false);
  }
  if (key.pixel_module) {
    add_stage(VK_SHADER_STAGE_FRAGMENT_BIT, key.pixel_module, true);
  }

  VkVertexInputBindingDescription bindings[kMaxVertexBindings];
  for (uint32_t i = 0; i < key.binding_count; ++i) {
    bindings[i] = {i, key.binding_strides[i], VK_VERTEX_INPUT_RATE_VERTEX};
  }
  VkVertexInputAttributeDescription attributes[kMaxVertexAttributes];
  for (uint32_t i = 0; i < key.attribute_count; ++i) {
    const auto& a = key.attributes[i];
    attributes[i] = {a.location, a.binding, VkFormat(a.format), a.offset};
  }
  VkPipelineVertexInputStateCreateInfo vertex_input = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = key.binding_count;
  vertex_input.pVertexBindingDescriptions = bindings;
  vertex_input.vertexAttributeDescriptionCount = key.attribute_count;
  vertex_input.pVertexAttributeDescriptions = attributes;

  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  input_assembly.topology = VkPrimitiveTopology(key.topology);
  input_assembly.primitiveRestartEnable = key.primitive_restart;

  VkPipelineViewportStateCreateInfo viewport = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VkPolygonMode(key.polygon_mode);
  raster.cullMode = key.cull_mode;
  raster.frontFace = VkFrontFace(key.front_face);
  raster.depthBiasEnable = key.depth_bias;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VkSampleCountFlagBits(key.sample_count);
  multisample.alphaToCoverageEnable = key.alpha_to_coverage;

  VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth_stencil.depthTestEnable = key.depth_test;
  depth_stencil.depthWriteEnable = key.depth_write;
  depth_stencil.depthCompareOp = VkCompareOp(key.depth_compare);
  depth_stencil.stencilTestEnable = key.stencil_test;
  auto stencil = [](const uint8_t ops[4]) {
    VkStencilOpState s = {};
    s.failOp = VkStencilOp(ops[0]);
    s.passOp = VkStencilOp(ops[1]);
    s.depthFailOp = VkStencilOp(ops[2]);
    s.compareOp = VkCompareOp(ops[3]);
    return s;
  };
  depth_stencil.front = stencil(key.stencil_front);
  depth_stencil.back = stencil(key.stencil_back);

  VkPipelineColorBlendAttachmentState blend[kMaxColorAttachments] = {};
  for (uint32_t i = 0; i < key.color_count; ++i) {
    const auto& b = key.blend[i];
    blend[i].blendEnable = b.enable;
    blend[i].srcColorBlendFactor = VkBlendFactor(b.src_color);
    blend[i].dstColorBlendFactor = VkBlendFactor(b.dst_color);
    blend[i].colorBlendOp = VkBlendOp(b.color_op);
    blend[i].srcAlphaBlendFactor = VkBlendFactor(b.src_alpha);
    blend[i].dstAlphaBlendFactor = VkBlendFactor(b.dst_alpha);
    blend[i].alphaBlendOp = VkBlendOp(b.alpha_op);
    blend[i].colorWriteMask = b.write_mask;
  }
  VkPipelineColorBlendStateCreateInfo color_blend = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color_blend.attachmentCount = key.color_count;
  color_blend.pAttachments = blend;

  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,         VK_DYNAMIC_STATE_BLEND_CONSTANTS,
      VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
      VK_DYNAMIC_STATE_STENCIL_REFERENCE,
  };
  VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic.pDynamicStates = dynamic_states;

  PassFormats formats;
  formats.color_count = key.color_count;
  for (uint32_t i = 0; i < key.color_count; ++i) {
    formats.color[i] = VkFormat(key.color_formats[i]);
  }
  formats.depth_stencil = VkFormat(key.depth_format);
  formats.samples = VkSampleCountFlagBits(key.sample_count);

  VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  PipelineTarget target;
  if (!rendering_->PreparePipeline(formats, info, target)) {
    return VK_NULL_HANDLE;
  }
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &input_assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth_stencil;
  info.pColorBlendState = &color_blend;
  info.pDynamicState = &dynamic;
  info.layout = layout_;

  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult result =
      dfn.vkCreateGraphicsPipelines(device_->device(), cache_, 1, &info, nullptr, &pipeline);
  if (result != VK_SUCCESS) {
    MANTRA_WARN("vkCreateGraphicsPipelines failed ({}) for VS {:X} PS {:X}", int32_t(result),
                key.vertex_module, key.pixel_module);
    return VK_NULL_HANDLE;
  }
  cache_changes_.fetch_add(1, std::memory_order_relaxed);
  MANTRA_DEBUG("pipeline: {} attributes, {} color, depth {}, topology {}{}", key.attribute_count, key.color_count, key.depth_format, key.topology,
               key.rect_list ? " (rect list)" : "");
  return pipeline;
}

}  // namespace mantra
