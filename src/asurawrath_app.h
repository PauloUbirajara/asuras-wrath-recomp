#pragma once

#include <rex/rex_app.h>
#include <rex/filesystem.h>

class AsurawrathApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<AsurawrathApp>(new AsurawrathApp(ctx, "asura_wrath_recomp", PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    std::filesystem::create_directories("cache");
    paths.cache_root = std::filesystem::absolute("cache");

    if (paths.game_data_root.empty()) {
      if (std::filesystem::exists("extracted")) {
        paths.game_data_root = std::filesystem::absolute("extracted");
      } else if (std::filesystem::exists("game_data")) {
        paths.game_data_root = std::filesystem::absolute("game_data");
      } else {
        paths.game_data_root = std::filesystem::current_path();
      }
    }
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }
};
