#include "mantra_log.h"
#include <rex/system/gpu_plugin.h>

#include "native_graphics_system.h"
#include "native_command_processor.h"

// Guest frames presented so far (the app's FPS counter reads it through
// dlsym, as the plugin is loaded with RTLD_LOCAL).
extern "C" REX_GPU_PLUGIN_EXPORT uint64_t mantra_guest_swap_count(void) {
  return rex::graphics::native::g_guest_swaps.load(std::memory_order_relaxed);
}

// Settings > Frame traces > Save the picture (0 off, 1 on), live.
extern "C" REX_GPU_PLUGIN_EXPORT void mantra_set_trace_capture(int enabled) {
  rex::graphics::native::g_trace_capture.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    MANTRA_ERROR("host ABI {}, plugin ABI {}", abi_version,
                 rex::system::kGpuPluginAbiVersion);
    return nullptr;
  }
  if (!info || info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    MANTRA_ERROR("invalid GpuCreateInfo");
    return nullptr;
  }
  return new rex::graphics::native::NativeGraphicsSystem();
}
