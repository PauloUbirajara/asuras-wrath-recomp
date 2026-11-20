// asura_wrath - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/flags.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <imgui.h>
#include <SDL3/SDL_events.h>
#ifdef __ANDROID__
#include <jni.h>
#include <SDL3/SDL_system.h>
#endif
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>

#include "platform/platform.h"
#include "platform/gpu_driver.h"

#include <rex/input/input_system.h>

#include "debug/script_input.h"
#include "game/frame_rate.h"
#include "game/skip_intro.h"
#include "game/graphics_effects.h"
#include "input/pad_remap.h"
#include "ui/game_assets.h"
#include "ui/game_font.h"
#include "ui/fps_counter.h"
#include "ui/game_toast.h"
#include "ui/launcher.h"
#include "ui/launcher_text.h"
#include "ui/notice_toast.h"
#include "ui/window_icon.h"

REXCVAR_DECLARE(bool, launcher);

class AsuraWrathApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  ~AsuraWrathApp() override {
    SDL_RemoveEventWatch(&AsuraWrathApp::BackWatch, this);
#ifdef __ANDROID__
    if (android_app_ == this) android_app_ = nullptr;
#endif
  }

#ifdef __ANDROID__
  // InGameMenuActivity closed (GameActivity.nativeMenuClosed, any thread):
  // the game may not get a foreground event under a translucent activity.
  static void NotifyMenuClosed() {
    if (AsuraWrathApp* app = android_app_) {
      app->app_context().CallInUIThreadDeferred([app] { app->OnMenuClosed(); });
    }
  }
#endif

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<AsuraWrathApp>(new AsuraWrathApp(ctx, "asura_wrath_recomp",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // Logs (and the traces in them) go to <Files folder>/logs.
  void OnConfigureLogging(rex::LogConfig& config) override {
    if (const auto files = asura::platform::FilesDir(); !files.empty()) {
      config.log_dir = files / "logs";
    }
    log_dir_ = config.log_dir;
    log_name_prefix_ = config.app_name + "_";
  }
  // This process's log: the given --log_file, else the numbered file the SDK
  // just created (the newest <app>_NNN.log). For Graphics > Logs and the name
  // of the game's log when Play starts it.
  void OnPostInitLogging() override {
    std::filesystem::path file;
    if (rex::cvar::HasNonDefaultValue("log_file")) {
      file = std::filesystem::absolute(std::string(REXCVAR_GET(log_file)));
    } else {
      std::error_code ec;
      std::filesystem::file_time_type newest;
      for (const auto& entry : std::filesystem::directory_iterator(log_dir_, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(log_name_prefix_, 0) != 0 || entry.path().extension() != ".log" ||
            name.find(".game.") != std::string::npos) {
          continue;
        }
        const auto time = entry.last_write_time(ec);
        if (file.empty() || time > newest) {
          file = entry.path();
          newest = time;
        }
      }
    }
    asura::ui::SetLogLocation(log_dir_, file);
  }
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Default to the in-repo mantra renderer; --gpu_plugin=xenos overrides.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "mantra";
    }
    // The SDK's input drivers, with controller remapping (input/pad_remap.h).
    config.input_factory = [](bool tool_mode) { return asura::input::CreateInputSystem(tool_mode); };

    // Launcher settings (GPU, vsync, fullscreen, keyboard) as defaults for
    // flags nothing else set. VSync defaults on: while any ImGui dialog is up
    // (the launcher, the achievement toast, SDK overlays) the presenter
    // repaints continuously, and with immediate/mailbox that runs uncapped
    // (500-1000 fps), taking GPU time from the game on integrated GPUs. The
    // game itself runs at 30 fps, so vsync costs nothing.
    asura::ui::ApplyLauncherDefaults(launcher_settings_);
    startup_settings_ = launcher_settings_;
#ifdef __ANDROID__
    // Settings > GPU driver (Android: a custom one: before the GPU is set up).
    asura::platform::LoadCustomGpuDriver(launcher_settings_.gpu_driver);
#endif
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
    // The F3 overlay's FPS (mantra's swap count).
    SetGuestFrameStats(asura::ui::GuestFrameStats);
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
    // The fonts come from the player's game files; extract them once (into
    // the user's cache) when the game folder is known.
    if (!asura::ui::HaveGameFonts()) {
      std::error_code ec;
      const std::filesystem::path game = launcher_settings_.game_data;
      if (!game.empty() && std::filesystem::is_regular_file(game / "default.xex", ec)) {
        std::string error;
        if (!asura::ui::ExtractUiFonts(game, asura::ui::UiCacheDir(), &error)) {
          REXLOG_WARN("Game fonts: {}", error);
        }
      }
    }
    // The achievement list (names, icons) for the launcher's Achievements
    // page, in the game's language: on Android the launcher can't read
    // default.xex itself, so the game writes it (in the background).
    if (asura::ui::CachedAchievementLanguage() != launcher_settings_.language &&
        launcher_settings_.language != 0) {
      std::error_code ec;
      const std::filesystem::path game = launcher_settings_.game_data;
      if (!game.empty() && std::filesystem::is_regular_file(game / "default.xex", ec)) {
        std::thread([game, language = launcher_settings_.language] {
          std::string error;
          if (!asura::ui::ExtractAchievementList(game, asura::ui::AchievementCacheDir(), language,
                                                 &error)) {
            REXLOG_WARN("Achievement list: {}", error);
          }
        }).detach();
      }
    }
    toast_font_ = asura::ui::AddGameFontFamily(atlas, "MediumFont");
    title_font_ = asura::ui::AddGameFontFamily(atlas, "TitleFont");
    // Until the game's fonts are extracted: the bundled Noto Serif (with the
    // launcher's Japanese).
    if (!toast_font_) {
      toast_font_ = asura::ui::AddBundledFont(atlas, false);
      title_font_ = asura::ui::AddBundledFont(atlas, true);
    }
  }
  // The SDK's overlays in the launcher's orange instead of green.
  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override {
    asura::ui::ApplyOrangeAccent(imgui_style, ui_style);
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
  // Game files swapped for copies (skip the logos, graphics effects): before
  // the game thread starts, which reads its config first thing.
  void OnPreLaunchModule() override {
    if (launcher_settings_.skip_intro) asura::game::SkipIntroMovies(runtime());
    asura::game::ApplyGraphicsEffects(runtime(), launcher_settings_.effects_off);
  }
  // Debug: ASURA_TEST_TOAST=N shows the game's first N achievements as toasts
  // (4.5 s each) once the title is running.
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    (void)thread;
    asura::game::StartFrameRateHook(runtime());
    DumpAchievementIcons();
    StartTraceWatcher();
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
  void OnShutdown() override {
    asura::game::StopFrameRateHook();
    fps_counter_.reset();
    trace_watcher_ = {};  // stops and joins
    notice_.reset();
  }
  void OnConfigurePaths(rex::PathConfig& paths) override {
    MigrateLegacyFolders(paths);
    // launcher.cfg next to the launcher (it names the Files folder, so it
    // can't live in it).
    launcher_settings_path_ = asura::ui::LauncherSettings::Locate(paths.user_data_root);
    launcher_settings_.Load(launcher_settings_path_);
    // The Files folder: saves and profile, caches (platform::CacheDir()),
    // logs. Everything that reads it (here, the mantra plugin, the game Play
    // starts) takes it from the environment. One set in the shell wins.
    if (!std::getenv(asura::platform::kDataDirVariable)) {
      const std::string files = launcher_settings_.data_folder.empty()
                                    ? paths.user_data_root.string()
                                    : launcher_settings_.data_folder;
      asura::platform::SetEnv(asura::platform::kDataDirVariable, files.c_str());
    }
    const std::filesystem::path files = asura::platform::FilesDir();
    paths.user_data_root = files;
    if (rex::cvar::GetFlagSource("cache_root") == rex::cvar::Source::kDefault) {
      paths.cache_root = asura::platform::CacheDir();
    }
    MoveLegacyCache();
    user_data_root_path_ = paths.user_data_root;
    // The config TOML next to the executable if there is one (portable
    // installs), else in the Files folder (the AppImage's folder is
    // read-only).
    std::error_code ec;
    if (!std::filesystem::exists(paths.config_path, ec)) {
      paths.config_path = paths.user_data_root / paths.config_path.filename();
    }
    // Game data > Traces: the file that makes mantra trace the next frame
    // (one set in the shell wins). The game Play starts inherits it.
    if (const char* trigger = std::getenv("MANTRA_TRACE_TRIGGER"); !trigger || !*trigger) {
      if (const auto dir = asura::platform::CacheDir(); !dir.empty()) {
        asura::platform::SetEnv("MANTRA_TRACE_TRIGGER", (dir / "trace.now").string().c_str());
      }
    }
    // Each traced frame's file (Game data > Traces), not the log.
    if (const auto traces = asura::ui::TraceDirectory(); !traces.empty()) {
      asura::platform::SetEnv("MANTRA_TRACE_DIR", traces.string().c_str());
    }
    // Game data > Logs > Detailed logs (off: the default level stays).
    if (launcher_settings_.detailed_logs) asura::ui::ApplyDetailedLogs(true);
    if (rex::cvar::GetFlagSource("game_data_root") == rex::cvar::Source::kDefault) {
      if (launcher_settings_.game_data.empty()) {
        // First start: an "extracted" folder in the working directory or
        // next to the executable.
        // (On Android ExePath() stands for the app's own storage folder.)
        for (const auto& dir : {std::filesystem::current_path(ec) / "extracted",
                                rex::filesystem::GetExecutableFolder() / "extracted",
                                asura::platform::ExePath().parent_path() / "extracted"}) {
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
  // loaded. This process stays the launcher; Play starts the game as another
  // process (this executable with --launcher=false), which takes the path
  // below.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    // The window is up: its icon (assets/ui/icon.png), launcher and game alike.
    asura::ui::ApplyWindowIcon();
    // Esc in game: the launcher over the running game. Registered before the
    // saved keys are applied, so a rebound key sticks.
    rex::ui::RegisterBind("bind_launcher", "Escape", "Open the launcher (in game)",
                          [this] { ToggleLauncherInGame(); });
    // Android's Back (and a keyboard's Back key): Esc's job, without being a
    // rebindable key. SDL calls the watch on the thread that queues the event
    // (Android's UI thread), so the work goes to this app's UI thread.
    SDL_AddEventWatch(&AsuraWrathApp::BackWatch, this);
#ifdef __ANDROID__
    android_app_ = this;
#endif
    // F3 in game: trace the next frame (Game data > Traces). The SDK's
    // performance overlay, F3 by default, gets no key (ApplyLauncherDefaults).
    rex::ui::RegisterBind("bind_trace", "F3", "Trace the next frame (in game)", [this] {
      // In the game; or in the launcher's window while the game Play
      // started runs (that process traces it).
      if ((game_running_ && !launcher_) || (launcher_ && launcher_->PlayRunning())) {
        asura::ui::RequestTrace();
      }
    });
    // Again now that the overlays registered their bind_* keys.
    asura::ui::ApplyLauncherDefaults(launcher_settings_);
    if (!LauncherWanted() || !imgui_drawer()) {
      asura::ui::ApplyWindowSettings(launcher_settings_);
      UpdateFpsCounter();
      game_running_ = true;
      UpdateCursor();
      return defaults;
    }
    launcher_ = std::make_unique<asura::ui::LauncherDialog>(
        imgui_drawer(), immediate_drawer(), window(), toast_font_, title_font_, launcher_settings_,
        launcher_settings_path_, user_data_root_path_,
        [context = &app_context()](std::function<void()> f) {
          context->CallInUIThreadDeferred(std::move(f));
        },
        [] { Restart(/*show_launcher=*/true); });
    // Settings' "Show an example" before the game runs (the SDK makes the
    // game's toast dialog once it starts).
    if (!toast_) {
      launcher_toast_ = std::make_unique<asura::ui::GameToastDialog>(
          imgui_drawer(), immediate_drawer(), nullptr, toast_font_,
          [context = &app_context()](std::function<void()> f) {
            context->CallInUIThreadDeferred(std::move(f));
          });
    }
    if (!asura::platform::CanStartSelf()) {
      // Android: Play starts the game here, as the SDK would have without
      // the launcher (the paths with the game folder chosen in it).
      launcher_->SetPlayInThisProcess(
          [this, defaults, resume = std::move(resume)](const asura::ui::LauncherSettings& s) {
            launcher_.reset();
            launcher_settings_ = s;
            startup_settings_ = s;
            asura::ui::ApplyLauncherDefaults(s);
            asura::ui::ApplyWindowSettings(s);
            asura::ui::ApplyLiveSettings(s);
            rex::PathConfig paths = defaults;
            if (!s.game_data.empty()) paths.game_data_root = s.game_data;
            UpdateFpsCounter();
            game_running_ = true;
            UpdateCursor();
            resume(paths);
          });
    }
    return std::nullopt;
  }

 private:
  // Enhancements > Mouse cursor: auto-hidden in game (the SDK shows it again
  // while the mouse moves).
  void UpdateCursor() {
    // Android: the touch controls step aside while the game menu is open.
    asura::input::SetTouchControlsSuspended(launcher_ != nullptr);
    if (!window() || !game_running_) return;
    window()->SetCursorVisibility(launcher_settings_.hide_cursor && !launcher_
                                      ? rex::ui::Window::CursorVisibility::kAutoHidden
                                      : rex::ui::Window::CursorVisibility::kVisible);
  }

  // Enhancements > Sound: mute while the window isn't focused (audio_mute is
  // read per audio buffer), unless the config file or command line set it.
  void OnWindowFocusChanged(bool focused) override {
    const auto source = rex::cvar::GetFlagSource("audio_mute");
    const bool ours = source == rex::cvar::Source::kDefault || source == rex::cvar::Source::kRuntime;
    if (!ours) return;
    const bool mute = !focused && launcher_settings_.mute_background && game_running_;
    if (mute != muted_in_background_) {
      muted_in_background_ = mute;
      rex::cvar::SetFlagByName("audio_mute", mute ? "true" : "false");
    }
  }

  // Graphics > "Show the frame rate".
  void UpdateFpsCounter() {
    if (launcher_settings_.show_fps && !fps_counter_ && imgui_drawer()) {
      fps_counter_ = std::make_unique<asura::ui::FpsCounterDialog>(imgui_drawer(), toast_font_);
    } else if (!launcher_settings_.show_fps) {
      fps_counter_.reset();
    }
  }

  static bool SDLCALL BackWatch(void* self, SDL_Event* e) {
    auto* app = static_cast<AsuraWrathApp*>(self);
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_AC_BACK &&
        !e->key.repeat) {
      app->app_context().CallInUIThreadDeferred([app] { app->OnBack(); });
    }
#ifdef __ANDROID__
    // Back from the Android menu (GameActivity.openMenu): its changes.
    if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
      app->app_context().CallInUIThreadDeferred([app] { app->OnMenuClosed(); });
    }
#endif
    return true;
  }

  // Back: the launcher's own Back (drawer, confirmations, resume), or, in
  // game, opens the launcher (on Android, the app's own menu: the Kotlin
  // launcher over the paused game).
  void OnBack() {
    if (launcher_) {
      launcher_->Back();
    } else {
      ToggleLauncherInGame();
    }
  }

#ifdef __ANDROID__
  // GameActivity.openMenu() (InGameMenuActivity, the launcher's pages with
  // Resume, Restart and Quit). False if the activity has no such method.
  bool OpenAndroidMenu() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID open = env->GetMethodID(cls, "openMenu", "()V");
    if (open) {
      env->CallVoidMethod(activity, open);
    }
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      open = nullptr;
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return open != nullptr;
  }

  // The game is back in front: the menu may have changed launcher.cfg (the
  // live settings apply now; the others at the next start, as the menu said),
  // or asked for this game menu (its "Game menu": the button remap,
  // achievements and the rest), leaving a note next to launcher.cfg.
  void OnMenuClosed() {
    std::error_code ec;
    const auto dir = launcher_settings_path_.parent_path();
    // Each note is for this close only: one left over (the game wasn't
    // running, or this menu was already open) must not open it on a later
    // Back.
    const bool open_game_menu = std::filesystem::remove(dir / ".open_game_menu", ec);
    const bool toast_example = std::filesystem::remove(dir / ".toast_example", ec);
    if (!game_running_ || launcher_) return;
    asura::ui::LauncherSettings s;
    if (s.Load(launcher_settings_path_)) {
      launcher_settings_ = s;
      asura::ui::ApplyLiveSettings(s);
      UpdateFpsCounter();
    }
    if (open_game_menu) {
      REXLOG_INFO("Android menu closed: opening the game menu");
      ToggleLauncherInGame(/*game_menu=*/true);
    }
    // Settings' "Show an example": the first achievement, as a toast.
    if (toast_example) {
      rex::system::AchievementEvent sample;
      const auto list = achievements().ListAchievements();
      if (!list.empty()) {
        sample.achievement = list.front();
      } else {
        sample.achievement.label = "Achievement";
        sample.achievement.gamerscore = 20;
      }
      asura::ui::PreviewToast(sample);
    }
  }
#endif

  // bind_launcher (Esc): opens the launcher over the game, or resumes from it.
  // The game keeps running underneath.
  // On Android every way in (Back, Esc, a pad's bind) opens the app's own
  // menu; only its "Game menu" row (game_menu) opens this one.
  void ToggleLauncherInGame(bool game_menu = false) {
    if (!game_running_ || !imgui_drawer()) return;
    // Esc also closes the SDK's console and settings overlays.
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) return;
    if (launcher_) {
      launcher_->Back();
      return;
    }
#ifdef __ANDROID__
    if (!game_menu) {
      if (OpenAndroidMenu()) return;
      REXLOG_WARN("GameActivity.openMenu failed; opening the game menu instead");
    }
#endif
    if (window()) window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    asura::input::SetTouchControlsSuspended(true);
    launcher_ = std::make_unique<asura::ui::LauncherDialog>(
        imgui_drawer(), immediate_drawer(), window(), toast_font_, title_font_, launcher_settings_,
        launcher_settings_path_, user_data_root_path_,
        [context = &app_context()](std::function<void()> f) {
          context->CallInUIThreadDeferred(std::move(f));
        },
        [] { Restart(/*show_launcher=*/true); });
    // The game folder and language the game runs with (the settings may ask
    // for others, from the next start): from before any in-game change.
    if (!running_captured_) {
      running_game_data_ = launcher_settings_.game_data;
      running_language_ = launcher_settings_.language;
      running_captured_ = true;
    }
    launcher_->SetInGame(
        [this](const asura::ui::LauncherSettings& s) {
          // Fixed once the game runs: the presenter's (NeedsRestart), the
          // game folder and the game's language.
          return asura::ui::NeedsRestart(startup_settings_, s) ||
                 s.game_data != running_game_data_ || s.language != running_language_;
        },
        [this](const asura::ui::LauncherSettings& s, bool restart) {
          launcher_.reset();
          if (restart) Restart(/*show_launcher=*/false);
          // Settings the presenter or the game fixed at start keep their
          // running values until the next start; the saved (and next
          // opened) settings are the chosen ones, so reopening the launcher
          // doesn't undo them.
          launcher_settings_ = s;
          asura::ui::ApplyWindowSettings(s);
          asura::ui::ApplyLiveSettings(s);
          UpdateFpsCounter();
          UpdateCursor();
        });
  }

  static bool LauncherWanted() {
    return REXCVAR_GET(launcher) && !std::getenv("ASURA_INPUT_SCRIPT");
  }

  // Runs this executable again with the same arguments, skipping the
  // launcher unless `show_launcher`. Only returns if exec fails.
  static void Restart(bool show_launcher) {
    std::vector<std::string> args = asura::platform::ExeArgs();
    if (args.empty()) return;
    if (!show_launcher) args.push_back("--launcher=false");
    REXLOG_INFO("Restarting ({})", show_launcher ? "launcher" : "graphics settings");
    rex::FlushLogging();
    asura::platform::ReplaceProcess(asura::platform::ExePath(), args);
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
    move_once(asura::platform::LegacyCacheDir());
  }

  // Builds before the single Files folder kept the caches in ~/.cache. Move
  // them into <Files folder>/cache once (unless it has its own already).
  static void MoveLegacyCache() {
    namespace fs = std::filesystem;
    const fs::path from = asura::platform::LegacyCacheDir(), to = asura::platform::CacheDir();
    std::error_code ec;
    if (from.empty() || to.empty() || fs::exists(to, ec) || !fs::is_directory(from, ec)) return;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec) {  // another drive: copy, then remove the original
      ec.clear();
      fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
      if (!ec) fs::remove_all(from, ec);
    }
    if (ec) {
      REXLOG_WARN("Could not move {} to {}: {}", from.string(), to.string(), ec.message());
    } else {
      REXLOG_INFO("Moved {} to {}", from.string(), to.string());
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
  std::unique_ptr<asura::ui::GameToastDialog> launcher_toast_;  // examples before the game runs

  // "Frame N traced" at the top of the screen when mantra finishes a trace
  // (F3 or Game data > Traces): <trigger>.done is rewritten with its number.
  void StartTraceWatcher() {
    const std::filesystem::path done = asura::ui::TraceDoneFile();
    if (done.empty() || !imgui_drawer() || trace_watcher_.joinable()) return;
    asura::ui::SetLauncherLanguage(launcher_settings_.language);
    trace_watcher_ = std::jthread([this, done](std::stop_token stop) {
      // By the frame number in the file, not its time: on filesystems with
      // coarse timestamps two traces within a second kept the same time and
      // the second showed nothing.
      auto read_frame = [&done] {
        unsigned long long frame = 0;
        if (std::ifstream in(done); in) in >> frame;
        return frame;
      };
      // (The time still counts: the same frame number can come again in a
      // later session.)
      std::error_code ec;
      unsigned long long last = read_frame();
      auto last_time = std::filesystem::last_write_time(done, ec);
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto time = std::filesystem::last_write_time(done, ec);
        if (ec) continue;
        const unsigned long long frame = read_frame();
        if (!frame || (frame == last && time == last_time)) continue;
        last = frame;
        last_time = time;
        // Settings > Frame traces > Show a notice.
        if (!asura::ui::GetTraceOptions().notice) continue;
        char text[96];
        std::snprintf(text, sizeof(text), asura::ui::T("Frame %llu traced"), frame);
        app_context().CallInUIThread([this, line = std::string(text)] {
          if (!notice_) {
            notice_ = std::make_unique<asura::ui::NoticeDialog>(
                imgui_drawer(), toast_font_, [context = &app_context()](std::function<void()> f) {
                  context->CallInUIThread(std::move(f));
                });
          }
          notice_->Show(line);
        });
      }
    });
  }
  std::unique_ptr<asura::ui::NoticeDialog> notice_;
  std::jthread trace_watcher_;

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
  bool running_captured_ = false;
  std::string running_game_data_;
  uint32_t running_language_ = 0;
  bool game_running_ = false;
  bool muted_in_background_ = false;  // audio_mute set by OnWindowFocusChanged  // past the launcher (bind_launcher works from then on)
  std::filesystem::path launcher_settings_path_;
  std::filesystem::path log_dir_;
  std::string log_name_prefix_;
  std::unique_ptr<asura::ui::LauncherDialog> launcher_;
  std::unique_ptr<asura::ui::FpsCounterDialog> fps_counter_;
#ifdef __ANDROID__
  static inline AsuraWrathApp* android_app_ = nullptr;
#endif
};

#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL
Java_org_asurawrath_recompiled_GameActivity_nativeMenuClosed(JNIEnv*, jclass) {
  AsuraWrathApp::NotifyMenuClosed();
}
#endif
