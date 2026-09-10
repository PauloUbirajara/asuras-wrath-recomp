#pragma once

#include <fstream>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#if defined(__ANDROID__)
#include <SDL3/SDL_hints.h>
#endif

namespace {
bool ExtractDiscEntry(rex::filesystem::Entry* entry, const std::filesystem::path& target_path) {
  std::error_code ec;
  if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
    std::filesystem::create_directories(target_path, ec);
    for (const auto& child : entry->children()) {
      auto child_target = target_path / child->name();
      if (!ExtractDiscEntry(child.get(), child_target)) {
        return false;
      }
    }
    return true;
  }

  auto parent_dir = target_path.parent_path();
  if (!parent_dir.empty()) {
    std::filesystem::create_directories(parent_dir, ec);
  }

  auto disc_entry = static_cast<rex::filesystem::DiscImageEntry*>(entry);
  if (!disc_entry || !disc_entry->mmap()) {
    return false;
  }

  std::ofstream out(target_path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }

  const char* data_ptr = reinterpret_cast<const char*>(disc_entry->mmap()->data() + disc_entry->data_offset());
  size_t data_size = disc_entry->data_size();
  if (data_size > 0) {
    out.write(data_ptr, data_size);
  }
  out.close();
  return out.good();
}

bool ExtractIsoToDirectory(const std::filesystem::path& iso_path, const std::filesystem::path& dest_dir) {
  bool success = false;
  std::error_code ec;
  std::filesystem::create_directories(dest_dir, ec);
  REXLOG_INFO("Extracting ISO {} to {}...", iso_path.string(), dest_dir.string());

  {
    rex::filesystem::DiscImageDevice device("game:", iso_path);
    if (!device.Initialize()) {
      REXLOG_ERROR("Failed to initialize DiscImageDevice for ISO extraction: {}", iso_path.string());
      return false;
    }

    const rex::filesystem::Entry* root = device.root();
    if (!root) {
      REXLOG_ERROR("ISO disc image root entry is null");
      return false;
    }

    success = true;
    for (const auto& child : root->children()) {
      auto child_target = dest_dir / child->name();
      if (!ExtractDiscEntry(child.get(), child_target)) {
        REXLOG_ERROR("Failed to extract entry {} from ISO", child->name());
        success = false;
        break;
      }
    }
  }

  if (success) {
    REXLOG_INFO("Successfully extracted ISO to {}", dest_dir.string());
  }
  return success;
}
}  // namespace

class AsurawrathApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<AsurawrathApp>(new AsurawrathApp(ctx, "asura_wrath_recomp", PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::error_code ec;
    auto cache_dir = paths.user_data_root / "cache";
    std::filesystem::create_directories(cache_dir, ec);
    paths.cache_root = cache_dir;

    auto resolve_game_dir = [](const std::filesystem::path& dir, std::filesystem::path& out_path) -> bool {
      std::error_code ec;
      if (dir.empty() || !std::filesystem::exists(dir, ec)) {
        return false;
      }
      if (std::filesystem::is_regular_file(dir, ec)) {
        auto ext = dir.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".iso" || ext == ".gdfx") {
          out_path = dir;
          return true;
        }
      }
      if (std::filesystem::exists(dir / "default.xex", ec)) {
        out_path = dir;
        return true;
      }
      if (std::filesystem::exists(dir / "BCGame" / "default.xex", ec)) {
        out_path = dir / "BCGame";
        return true;
      }
      if (std::filesystem::exists(dir / "extracted", ec)) {
        auto extracted_dir = dir / "extracted";
        if (std::filesystem::exists(extracted_dir / "default.xex", ec)) {
          out_path = extracted_dir;
          return true;
        }
        if (std::filesystem::exists(extracted_dir / "BCGame" / "default.xex", ec)) {
          out_path = extracted_dir / "BCGame";
          return true;
        }
        std::filesystem::directory_iterator end_it;
        for (std::filesystem::directory_iterator entry(extracted_dir, ec); entry != end_it && !ec; entry.increment(ec)) {
          if (entry->is_regular_file(ec)) {
            auto ext = entry->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".iso" || ext == ".gdfx") {
              out_path = entry->path();
              return true;
            }
          }
        }
        out_path = extracted_dir;
        return true;
      }
      if (std::filesystem::exists(dir / "game_data", ec)) {
        out_path = dir / "game_data";
        return true;
      }
      std::filesystem::directory_iterator end_it;
      for (std::filesystem::directory_iterator entry(dir, ec); entry != end_it && !ec; entry.increment(ec)) {
        if (entry->is_regular_file(ec)) {
          auto ext = entry->path().extension().string();
          std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
          if (ext == ".iso" || ext == ".gdfx") {
            out_path = entry->path();
            return true;
          }
        }
      }
      return false;
    };

    std::filesystem::path resolved;
    if (!paths.game_data_root.empty() && resolve_game_dir(paths.game_data_root, resolved)) {
      paths.game_data_root = resolved;
    } else {
      auto cwd = std::filesystem::current_path(ec);
      const std::filesystem::path search_dirs[] = {
          paths.user_data_root,
          paths.user_data_root.parent_path(),
          cwd
      };

      for (const auto& dir : search_dirs) {
        if (resolve_game_dir(dir, resolved)) {
          paths.game_data_root = resolved;
          break;
        }
      }
    }

    auto cwd = std::filesystem::current_path(ec);
    if (paths.game_data_root.empty()) {
      paths.game_data_root = paths.user_data_root.empty() ? cwd : paths.user_data_root;
    }

    if (std::filesystem::is_regular_file(paths.game_data_root, ec)) {
      auto ext = paths.game_data_root.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".iso" || ext == ".gdfx") {
        auto iso_path = paths.game_data_root;
        auto dest_dir = paths.user_data_root.empty() ? iso_path.parent_path() / "extracted"
                                                     : paths.user_data_root / "extracted";
        if (!std::filesystem::exists(dest_dir / "default.xex", ec) &&
            !std::filesystem::exists(dest_dir / "BCGame" / "default.xex", ec)) {
          if (ExtractIsoToDirectory(iso_path, dest_dir)) {
            std::filesystem::remove(iso_path, ec);
            std::filesystem::remove(paths.user_data_root / "Asura's Wrath.iso", ec);
            std::filesystem::remove(dest_dir / "Asura's Wrath.iso", ec);
            std::filesystem::path new_resolved;
            if (resolve_game_dir(dest_dir, new_resolved)) {
              paths.game_data_root = new_resolved;
            } else {
              paths.game_data_root = dest_dir;
            }
          }
        } else {
          std::filesystem::remove(iso_path, ec);
          std::filesystem::remove(paths.user_data_root / "Asura's Wrath.iso", ec);
          std::filesystem::remove(dest_dir / "Asura's Wrath.iso", ec);
          std::filesystem::path new_resolved;
          if (resolve_game_dir(dest_dir, new_resolved)) {
            paths.game_data_root = new_resolved;
          } else {
            paths.game_data_root = dest_dir;
          }
        }
      }
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
#if defined(__ANDROID__)
    SDL_SetHint(SDL_HINT_ANDROID_ALLOW_RECREATE_ACTIVITY, "1");
#endif
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }
};
