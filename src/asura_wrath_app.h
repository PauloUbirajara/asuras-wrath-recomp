// asura_wrath - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/flags.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>

#include <rex/input/input_system.h>

#include "debug/script_input.h"
#include "ui/game_font.h"
#include "ui/game_toast.h"

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
  void OnPostSetup() override {
    // Debug: ASURA_INPUT_SCRIPT drives a scripted gamepad (debug/script_input.h).
    if (auto driver = asura::debug::ScriptInputDriver::FromEnvironment()) {
      if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
        driver->Setup();
        input->AddDriver(std::move(driver));
      }
    }
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;

  // Achievement toast in the game's look (ui/game_toast.h): the game's serif
  // bitmap font (from the game data, see ui/game_font.h), optional
  // background/portrait art, the achievement's icon.
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    toast_font_ = asura::ui::AddGameFont(atlas, asura::ui::GameUiFile("MediumFont.awf"));
  }
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog()
      override {
    if (!imgui_drawer() || !immediate_drawer() || !runtime()) {
      return nullptr;
    }
    auto toast = std::make_unique<asura::ui::GameToastDialog>(
        imgui_drawer(), immediate_drawer(), runtime(), toast_font_,
        [context = &app_context()](std::function<void()> function) {
          context->CallInUIThread(std::move(function));
        });
    toast_ = toast.get();
    return toast;
  }
  // Debug: ASURA_TEST_TOAST=N shows the game's first N achievements as toasts
  // (4.5 s each) once the title is running.
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    (void)thread;
    DumpAchievementIcons();
    const char* env = std::getenv("ASURA_TEST_TOAST");
    if (!env || !toast_) return;
    const auto list = achievements().ListAchievements();
    const int count = std::max(1, std::atoi(env));
    for (int i = 0; i < count; ++i) {
      rex::system::AchievementEvent event;
      if (!list.empty()) {
        event.achievement = list[size_t(i) % list.size()];
      } else {
        event.achievement.label = "Wrath of a God";
        event.achievement.gamerscore = 30;
      }
      toast_->Push(event);
    }
  }
  // void OnShutdown() override {}
  void OnConfigurePaths(rex::PathConfig& paths) override {
    user_data_root_path_ = paths.user_data_root;
  }

 private:
  // Debug: ASURA_DUMP_ACHIEVEMENTS=<dir> writes the title's achievement icons
  // (XDBF images, PNG) as <id>.png, the title icon as title.png, and an
  // achievements.tsv (id, image, gamerscore, name, description).
  void DumpAchievementIcons() {
    const char* dir = std::getenv("ASURA_DUMP_ACHIEVEMENTS");
    if (!dir || !*dir || !runtime() || !runtime()->kernel_state()) return;
    const auto db = runtime()->kernel_state()->title_xdbf();
    if (!db.is_valid()) return;
    const std::filesystem::path out(dir);
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    auto write_image = [&](uint64_t image_id, const std::string& name) {
      const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, image_id);
      if (!block) return;
      std::ofstream(out / name, std::ios::binary)
          .write(reinterpret_cast<const char*>(block.buffer), std::streamsize(block.size));
    };
    write_image(0x8000, "title.png");
    std::ofstream tsv(out / "achievements.tsv");
    tsv << "id\timage\tgamerscore\tname\tdescription\n";
    for (const auto& a : achievements().ListAchievements()) {
      write_image(a.image_id, std::to_string(a.id) + ".png");
      tsv << a.id << '\t' << a.image_id << '\t' << a.gamerscore << '\t' << a.label << '\t'
          << a.description << '\n';
    }
    REXLOG_INFO("Dumped achievement icons to {}", out.string());
  }

  ImFont* toast_font_ = nullptr;
  asura::ui::GameToastDialog* toast_ = nullptr;  // owned by the SDK

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
