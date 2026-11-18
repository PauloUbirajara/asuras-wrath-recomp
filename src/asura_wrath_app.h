// asura_wrath - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
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
#include "ui/launcher.h"

REXCVAR_DECLARE(bool, launcher);

class AsuraWrathApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<AsuraWrathApp>(new AsuraWrathApp(ctx, "asura_wrath_recomp",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // Logs go to <exe dir>/logs (the SDK default) when that folder is
  // writable, else to <user data>/logs (the AppImage's folder is read-only).
  void OnConfigureLogging(rex::LogConfig& config) override {
    if (!user_data_root_path_.empty() &&
        access(config.log_dir.parent_path().c_str(), W_OK) != 0) {
      config.log_dir = user_data_root_path_ / "logs";
    }
  }
  // void OnPostInitLogging() override {}
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Default to the in-repo mantra renderer; --gpu_plugin=xenos overrides.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "mantra";
    }

    // Launcher settings (GPU, vsync, fullscreen, keyboard) as defaults for
    // flags nothing else set. VSync defaults on: while any ImGui dialog is up
    // (the launcher, the achievement toast, SDK overlays) the presenter
    // repaints continuously, and with immediate/mailbox that runs uncapped
    // (500-1000 fps), taking GPU time from the game on integrated GPUs. The
    // game itself runs at 30 fps, so vsync costs nothing.
    asura::ui::ApplyLauncherDefaults(launcher_settings_);
    startup_settings_ = launcher_settings_;
    // The launcher opens in a window; Play applies the fullscreen setting.
    if (LauncherWanted()) {
      asura::ui::UseWindowForLauncher();
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
    title_font_ = asura::ui::AddGameFont(atlas, asura::ui::GameUiFile("TitleFont.awf"));
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
    WriteAchievementCache();
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
    MigrateLegacyFolders(paths);
    user_data_root_path_ = paths.user_data_root;
    // The config TOML next to the executable if there is one (portable
    // installs), else in the user data folder (the AppImage's folder is
    // read-only).
    std::error_code ec;
    if (!std::filesystem::exists(paths.config_path, ec)) {
      paths.config_path = paths.user_data_root / paths.config_path.filename();
    }
    launcher_settings_path_ = asura::ui::LauncherSettings::PathIn(paths.user_data_root);
    launcher_settings_.Load(launcher_settings_path_);
    if (rex::cvar::GetFlagSource("game_data_root") == rex::cvar::Source::kDefault) {
      if (launcher_settings_.game_data.empty()) {
        // First start: an "extracted" folder in the working directory or
        // next to the executable.
        for (const auto& dir : {std::filesystem::current_path(ec) / "extracted",
                                rex::filesystem::GetExecutableFolder() / "extracted"}) {
          if (std::filesystem::is_regular_file(dir / "default.xex", ec)) {
            launcher_settings_.game_data = dir.string();
            break;
          }
        }
      }
      if (!launcher_settings_.game_data.empty()) {
        paths.game_data_root = launcher_settings_.game_data;
      }
    } else {
      launcher_settings_.game_data = paths.game_data_root.string();
    }
  }

  // The launcher (ui/launcher.h) runs here: the window is up, the game isn't
  // loaded yet. Play resumes initialization with the chosen game folder.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    if (!LauncherWanted() || !imgui_drawer()) {
      asura::ui::ApplyWindowSettings(launcher_settings_);
      return defaults;
    }
    launcher_ = std::make_unique<asura::ui::LauncherDialog>(
        imgui_drawer(), immediate_drawer(), window(), toast_font_, title_font_, launcher_settings_,
        launcher_settings_path_, user_data_root_path_,
        [context = &app_context()](std::function<void()> f) {
          context->CallInUIThreadDeferred(std::move(f));
        },
        [this, defaults, resume = std::move(resume)](const asura::ui::LauncherSettings& s) {
          launcher_.reset();
          // GPU, present mode and cropping are fixed once the window's
          // presenter exists: start over with the new settings.
          if (asura::ui::NeedsRestart(startup_settings_, s)) {
            Restart();
          }
          launcher_settings_ = s;
          // Window settings first: they go through the flag registry (the
          // window follows), the rest are plain defaults.
          asura::ui::ApplyWindowSettings(s);
          asura::ui::ApplyLauncherDefaults(s);
          rex::PathConfig paths = defaults;
          if (rex::cvar::GetFlagSource("game_data_root") == rex::cvar::Source::kDefault) {
            paths.game_data_root = s.game_data;
          }
          resume(paths);
        });
    return std::nullopt;
  }

 private:
  static bool LauncherWanted() {
    return REXCVAR_GET(launcher) && !std::getenv("ASURA_INPUT_SCRIPT");
  }

  // The achievement list and icons for the launcher's Achievements page
  // (asura::ui::AchievementCacheDir): the launcher runs before the game is
  // loaded, and the list only exists in the (encrypted) executable.
  void WriteAchievementCache() {
    if (!runtime() || !runtime()->kernel_state()) return;
    const auto db = runtime()->kernel_state()->title_xdbf();
    const auto list = achievements().ListAchievements();
    const std::filesystem::path dir = asura::ui::AchievementCacheDir();
    if (!db.is_valid() || list.empty() || dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto clean = [](std::string s) {
      std::replace_if(s.begin(), s.end(), [](char c) { return c == '\t' || c == '\n' || c == '\r'; },
                      ' ');
      return s;
    };
    std::ofstream tsv(dir / "achievements.tsv", std::ios::trunc);
    tsv << "id\tgamerscore\tname\tdescription\tlocked_description\n";
    for (const auto& a : list) {
      tsv << a.id << '\t' << a.gamerscore << '\t' << clean(a.label) << '\t'
          << clean(a.description) << '\t' << clean(a.unachieved_description) << '\n';
      const auto icon = dir / (std::to_string(a.id) + ".png");
      if (std::filesystem::exists(icon, ec)) continue;
      const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, a.image_id);
      if (block) {
        std::ofstream(icon, std::ios::binary)
            .write(reinterpret_cast<const char*>(block.buffer), std::streamsize(block.size));
      }
    }
  }

  // Runs this executable again with the same arguments, skipping the
  // launcher. Only returns if exec fails.
  static void Restart() {
    std::vector<std::string> args;
    if (std::ifstream in("/proc/self/cmdline", std::ios::binary); in) {
      std::string arg;
      while (std::getline(in, arg, '\0')) args.push_back(arg);
    }
    if (args.empty()) return;
    args.push_back("--launcher=false");
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    REXLOG_INFO("Restarting to apply the graphics settings");
    rex::FlushLogging();
    execv("/proc/self/exe", argv.data());
    REXLOG_ERROR("Restart failed; graphics settings apply on the next start");
  }

  // Builds before the rename used the folder name "asura_wrath" (user data
  // under ~/.local/share, caches under ~/.cache). Move them to the
  // "asura_wrath_recomp" names once, so saves and the shader cache survive.
  static void MigrateLegacyFolders(const rex::PathConfig& paths) {
    auto move_once = [](const std::filesystem::path& to) {
      if (to.empty() || to.filename() != "asura_wrath_recomp") return;
      const std::filesystem::path from = to.parent_path() / "asura_wrath";
      std::error_code ec;
      if (std::filesystem::exists(to, ec) || !std::filesystem::is_directory(from, ec)) return;
      std::filesystem::rename(from, to, ec);
      if (ec) {
        REXLOG_WARN("Could not move {} to {}: {}", from.string(), to.string(), ec.message());
      } else {
        REXLOG_INFO("Moved {} to {}", from.string(), to.string());
      }
    };
    move_once(paths.user_data_root);
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
      move_once(std::filesystem::path(xdg) / "asura_wrath_recomp");
    } else if (const char* home = std::getenv("HOME"); home && *home) {
      move_once(std::filesystem::path(home) / ".cache" / "asura_wrath_recomp");
    }
  }

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

  ImFont* toast_font_ = nullptr;  // MediumFont, also the launcher's text
  ImFont* title_font_ = nullptr;  // TitleFont, the launcher's titles
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
  asura::ui::LauncherSettings launcher_settings_, startup_settings_;
  std::filesystem::path launcher_settings_path_;
  std::unique_ptr<asura::ui::LauncherDialog> launcher_;
};
