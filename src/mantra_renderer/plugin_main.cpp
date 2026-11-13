#include "mantra_log.h"
#include <rex/system/gpu_plugin.h>

#include "native_graphics_system.h"

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
