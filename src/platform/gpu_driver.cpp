#include "gpu_driver.h"

#if defined(ASURA_ADRENOTOOLS)
#include <dlfcn.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <adrenotools/driver.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/instance.h>
#endif

namespace asura::platform {

#if defined(ASURA_ADRENOTOOLS)
namespace {

// meta.json's "libraryName" (the driver .so in the same folder).
std::string LibraryName(const std::filesystem::path& meta) {
  std::ifstream in(meta);
  const std::string text((std::istreambuf_iterator<char>(in)), {});
  const size_t key = text.find("\"libraryName\"");
  if (key == std::string::npos) return {};
  const size_t open = text.find('"', text.find(':', key) + 1);
  if (open == std::string::npos) return {};
  const size_t close = text.find('"', open + 1);
  if (close == std::string::npos) return {};
  return text.substr(open + 1, close - open - 1);
}

}  // namespace

bool LoadCustomGpuDriver(const std::string& name) {
  namespace fs = std::filesystem;
  if (name.empty()) return false;
  const char* drivers = std::getenv("ASURA_GPU_DRIVERS");
  const char* hooks = std::getenv("ASURA_NATIVE_LIB_DIR");
  if (!drivers || !*drivers || !hooks || !*hooks) {
    REXLOG_WARN("GPU driver {}: the app didn't say where drivers are; using the system's", name);
    return false;
  }
  const fs::path dir = fs::path(drivers) / name;
  const std::string library = LibraryName(dir / "meta.json");
  std::error_code ec;
  if (library.empty() || !fs::is_regular_file(dir / library, ec)) {
    REXLOG_WARN("GPU driver {}: no meta.json or driver library in {}; using the system's", name,
                dir.string());
    return false;
  }
  // The driver's own files (its shader cache) go here instead of /data/local.
  const fs::path redirect = fs::path(drivers) / "file_redirect";
  fs::create_directories(redirect, ec);
  const std::string driver_dir = dir.string() + "/";      // adrenotools appends the name
  const std::string redirect_dir = redirect.string() + "/";
  void* loader = adrenotools_open_libvulkan(
      RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM | ADRENOTOOLS_DRIVER_FILE_REDIRECT, nullptr, hooks,
      driver_dir.c_str(), library.c_str(), redirect_dir.c_str(), nullptr);
  if (!loader) {
    REXLOG_WARN("GPU driver {} ({}): adrenotools couldn't load it; using the system's", name,
                library);
    return false;
  }
  auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(loader, "vkDestroyInstance"));
  if (!get_proc || !destroy) {
    REXLOG_WARN("GPU driver {}: the hooked loader has no entry points; using the system's", name);
    return false;
  }
  rex::ui::vulkan::VulkanInstance::SetLoaderOverride(get_proc, destroy);
  REXLOG_INFO("GPU driver: {} ({})", name, library);
  return true;
}
#else
bool LoadCustomGpuDriver(const std::string&) { return false; }
#endif

}  // namespace asura::platform
