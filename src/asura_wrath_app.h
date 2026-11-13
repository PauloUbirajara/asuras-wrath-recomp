// asura_wrath - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>
#include <system_error>

#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/flags.h>

class AsuraWrathApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<AsuraWrathApp>(new AsuraWrathApp(ctx, "asura_wrath",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Default to the in-repo mantra renderer; --gpu_plugin=xenos overrides.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "mantra";
    }

    // First run: the game finds no save and asks "saved data does not exist.
    // Create saved data in selected storage device?" (default button = yes).
    // Answer XAM prompts with their default for this session so the save is
    // created without the dialog. Later sessions show prompts normally.
    if (!SaveDataExists()) {
      REXLOG_INFO("No Asura's Wrath save found; auto-accepting prompts to create it");
      REXCVAR_SET(headless, true);
    }
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  void OnConfigurePaths(rex::PathConfig& paths) override {
    user_data_root_path_ = paths.user_data_root;
  }

 private:
  // Saves live at <user_data>/<profile xuid>/43430817/00000001/SAVEDATA.DAT
  // (title id 43430817, content type 1 = saved game). Any profile counts.
  bool SaveDataExists() const {
    std::error_code ec;
    if (user_data_root_path_.empty() ||
        !std::filesystem::is_directory(user_data_root_path_, ec)) {
      return false;
    }
    for (const auto& profile : std::filesystem::directory_iterator(user_data_root_path_, ec)) {
      if (std::filesystem::exists(
              profile.path() / "43430817" / "00000001" / "SAVEDATA.DAT", ec)) {
        return true;
      }
    }
    return false;
  }

  std::filesystem::path user_data_root_path_;
};
