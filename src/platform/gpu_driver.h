// Custom Vulkan drivers on Android (Snapdragon / Adreno; Settings > GPU driver): Mesa Turnip or a
// newer Qualcomm driver, packed as the usual driver .zip (meta.json naming
// the library, plus the .so), installed by the launcher
// (LauncherActivity.kt, GpuDrivers.kt) into the app's internal storage:
//
//   $ASURA_GPU_DRIVERS/<name>/meta.json, <libraryName>.so
//
// launcher.cfg's gpu_driver=<name> picks one; empty is the phone's own.
// libadrenotools loads the system Vulkan loader in a namespace of its own,
// hooked to open that driver instead of the built-in one, and the SDK's
// VulkanInstance is given its entry points. arm64 only; elsewhere this does
// nothing.
#pragma once

#include <string>

namespace asura::platform {

// Call before the GPU is set up (OnPreSetup). Returns false (and the system
// driver stays) if `name` is empty, unknown or fails to load.
bool LoadCustomGpuDriver(const std::string& name);

}  // namespace asura::platform
