#pragma once
// Launcher shown before the game starts (AsuraWrathApp::OnFinalizePaths), in
// a window: a sidebar (Controls, Achievements, Saves, Settings, About,
// Play) and a content panel, in the language chosen in Settings
// (launcher_text.h). Settings are kept in <user data>/launcher.cfg and applied at startup
// as defaults, so command-line flags and the config TOML still win.
//
// Text uses the game's fonts when they were extracted (MediumFont.awf,
// TitleFont.awf; see ui/game_font.h).
// Optional art (found by GameUiFile, e.g. the repository's assets/ui):
//   logo.png            the game's logo at the top of the sidebar, instead of
//                       the title text (launcher_logo.png also works)
//   launcher_background.png  behind the content panel, nine-sliced (its frame
//                       keeps its proportions; content goes in the inner panel)
//   menu_button.png     sidebar buttons, tiled horizontally
//   launcher_title.png  art at the top of the sidebar, under the logo
// Skip it with --launcher=false (also skipped for scripted test runs).

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/style.h>
#include <rex/ui/window_listener.h>

#include "save_data.h"

struct SDL_Gamepad;

namespace rex::input {
struct X_INPUT_GAMEPAD;
}

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
class Window;
}  // namespace rex::ui

namespace asura::ui {

// How the game's 16:9 image fills a screen of another shape (ultrawide).
enum class AspectMode { kFit, kFill, kStretch };

struct LauncherSettings {
  int gpu = -1;  // vulkan_device, -1 = automatic
  bool vsync = true;
  bool fullscreen = true;
  bool exclusive = false;  // fullscreen switches the display mode (window size) instead of borderless
  int monitor = 0;         // 1-based, 0 = the default one
  bool keyboard = false;  // mnk_mode
  AspectMode aspect = AspectMode::kFit;
  int window_width = 0, window_height = 0;  // windowed size, 0 = default (1280x720)
  uint32_t language = 1;                    // user_language (XLanguage), 1 = English
  bool achievements_grid = false;           // Achievements page: grid instead of list
  std::string game_data;                    // game_data_root, empty = flag / default
  std::map<std::string, std::string> binds;  // keybind_* flag -> keys
  std::map<std::string, std::string> pad;    // controller remap: target -> source (pad_remap.h)
  double deadzone_left = 0, deadzone_right = 0;  // stick deadzones, fraction of the range
  int vibration = 100;                       // controller vibration strength, percent
  bool show_fps = false;                     // frame rate counter on screen
  int fps_corner = 0;                        // 0 top left, 1 top right, 2 bottom left, 3 bottom right
  int fps_cap = 30;                          // frame rate limit, 30 = the game's, 0 = unlimited
  bool skip_intro = false;                   // skip the logos and the autosave notice at start
  uint32_t turbo = 0;                        // Turbo: held controls repeat (bit = PadControl)
  bool hide_cursor = true;                   // mouse cursor hides while it isn't moving, in game
  bool mute_background = false;              // no sound while the window isn't focused
  int render_height = 720;                   // internal resolution (16:9), 720 = the game's
  // Graphics > "Higher resolution" (experimental): off, the game renders at
  // its own 720p whatever render_height says (RenderHeight).
  bool native_render = false;
  bool toasts = true;                        // achievement pop-ups
  int toast_corner = 3;                      // 0 top left, 1 top right, 2 bottom left, 3 bottom right
  double toast_seconds = 4.5;                // how long each pop-up stays

  static std::filesystem::path PathIn(const std::filesystem::path& user_data_root);
  bool Load(const std::filesystem::path& path);  // false if missing
  bool Save(const std::filesystem::path& path) const;
};

// Applies the settings to flags nothing else set (command line, environment,
// config TOML). Call before the GPU plugin is set up (OnPreSetup).
void ApplyLauncherDefaults(const LauncherSettings& settings);
// The controller remap in the settings.
std::array<uint8_t, 16> PadMapFrom(const LauncherSettings& settings);
// Opens the window (not fullscreen) for the launcher, unless the fullscreen
// flag was set elsewhere. Call after ApplyLauncherDefaults.
void UseWindowForLauncher();
// After Play: applies the window settings (size, fullscreen, aspect) live.
void ApplyWindowSettings(const LauncherSettings& settings);
// While the game runs (the launcher opened with Esc): the settings that can
// change without a restart (keys, controller, pop-ups); the window ones go
// through ApplyWindowSettings.
void ApplyLiveSettings(const LauncherSettings& settings);
// Settings that only apply when the game starts over (GPU, present mode,
// cropping).
// Whether the shader cache works with the given settings.
enum class ShaderCacheFit { kReady, kMissing, kOutdated };
ShaderCacheFit ShaderCacheFitFor(const LauncherSettings& s);
// The render height mantra uses: render_height with "Higher resolution" on,
// else the game's 720.
int RenderHeight(const LauncherSettings& s);
bool NeedsRestart(const LauncherSettings& before, const LauncherSettings& after);

// Turns the SDK's green ImGui accents (all overlays) orange.
void ApplyOrangeAccent(ImGuiStyle& imgui_style, rex::ui::Style& ui_style);

// mantra's shader cache location (MANTRA_SHADER_CACHE or the XDG cache).
std::filesystem::path ShaderCachePath();
// Shaders the game patched at runtime, kept by mantra for the next cache
// build (<cache dir>/shader_dumps; plus the repository's out/mantra_dumps).
std::vector<std::filesystem::path> ShaderDumpDirs();
// How many of those are newer than the cache (a rebuild adds them).
size_t ShadersNotInCache();

// The log folder and this process's log file (the app sets them once logging
// is up). The game started by Play logs next to it: <name>.game.log.
void SetLogLocation(const std::filesystem::path& dir, const std::filesystem::path& file);
const std::filesystem::path& LogDirectory();
const std::filesystem::path& LogFile();
// Whether the game's UI fonts were found (else ImGui's font is used).
bool HaveGameFonts();

class LauncherDialog : public rex::ui::ImGuiDialog,
                       public rex::ui::WindowListener,
                       public rex::ui::WindowInputListener {
 public:
  // Play starts the game as a separate process (this executable with
  // --launcher=false, in its own window) and becomes Stop while it runs.
  LauncherDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
                 rex::ui::Window* window, ImFont* font, ImFont* title_font,
                 LauncherSettings settings,
                 std::filesystem::path settings_path, std::filesystem::path user_data_root,
                 std::function<void(std::function<void()>)> call_in_ui_thread_deferred,
                 std::function<void()> on_relaunch);
  ~LauncherDialog() override;

  // Opened over the running game (Esc, in the game's process): Play becomes
  // Resume, or Restart when `needs_restart` says a change needs one.
  // `on_resume` (restart = the player chose Restart) runs on the UI thread
  // after the frame with the click; the launcher is detached by then and may
  // be destroyed by it.
  void SetInGame(std::function<bool(const LauncherSettings&)> needs_restart,
                 std::function<void(const LauncherSettings&, bool restart)> on_resume);
  // Esc again: like Resume (changes that need a restart wait for the next start).
  void RequestResume() { resume_requested_ = true; }

  // WindowListener / WindowInputListener.
  void OnFileDrop(rex::ui::FileDropEvent& e) override;
  void OnKeyDown(rex::ui::KeyEvent& e) override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class Page { kControls, kAchievements, kSaves, kSettings, kAbout };
  struct Image {
    std::unique_ptr<rex::ui::ImmediateTexture> texture;
    float width = 0, height = 0;
    // The visible (non-transparent) part, in UVs.
    ImVec2 uv0{0, 0}, uv1{1, 1};
  };
  struct GpuInfo {
    std::string name;
    std::string driver;  // e.g. "NVIDIA 580.95.05", "Intel open-source Mesa driver Mesa 25.2.3"
    std::string vulkan;  // the device's Vulkan version
  };
  struct Achievement {
    uint32_t id = 0, gamerscore = 0;
    std::string name, description, locked_description;
    Image icon;
    uint64_t unlocked_filetime = 0;  // 0 = locked
  };

  // A child process (mantra_shadergen) run in the background.
  struct ShaderBuild {
    enum State { kIdle, kRunning, kDone, kFailed };
    std::atomic<int> state{kIdle};
    int pid = -1;
    std::thread reader;
    std::mutex mutex;
    std::string last_line;  // under mutex
    std::atomic<int> done{0}, total{0};  // shaders translated ("translated N/M" lines)
    // update: keep the shaders `out` already has, translate only new ones.
    void Start(const std::filesystem::path& tool, const std::filesystem::path& game,
               const std::filesystem::path& out, const std::vector<std::filesystem::path>& dumps,
               bool update = false);
    void Stop();
    std::string Line();
  };

  // Copies the game files out of a disc image in the background.
  struct IsoExtract {
    enum State { kIdle, kRunning, kDone, kFailed };
    std::atomic<int> state{kIdle};
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> done_bytes{0}, total_bytes{0};
    std::thread worker;
    std::mutex mutex;
    std::string message;  // under mutex: current file or the error
    std::filesystem::path out;
    void Start(const std::filesystem::path& iso, const std::filesystem::path& out);
    void Stop();
    std::string Message();
  };

  // Reads game files the launcher shows (UI fonts, achievement list) in the
  // background.
  struct AssetJob {
    std::atomic<bool> running{false}, finished{false};
    std::thread worker;
    std::mutex mutex;
    std::string error;  // under mutex
    void Start(std::filesystem::path game, uint32_t language, bool fonts, bool achievements);
    void Join();
  };

  // The save under the user data folder: <profile>/43430817/00000001/<save>/,
  // named by <profile>/43430817/Headers/00000001/<save>.header.
  struct SaveInfo {
    std::string name, display_name;
    std::filesystem::path path, header;
    uint64_t bytes = 0;
    std::string modified;
    std::vector<std::pair<std::string, uint64_t>> files;  // name, size
    bool has_progress = false;
    SaveSummary progress;
  };
  struct ProfileInfo {
    std::string xuid;
    std::vector<SaveInfo> saves;
  };

  Image LoadImage(const std::filesystem::path& path, bool repeat = false);
  void StartAssetJob();
  void ScanSaves();
  void DrawSaves(float scale);
  void DrawStory(const SaveSummary& progress, float label_w, float scale);
  void DrawSettings(float scale);
  void LoadAchievements();
  void UseDroppedPath(const std::filesystem::path& path);
  void DrawSidebar(const ImVec2& pos, const ImVec2& size, float scale);
  void DrawPanel(const ImVec2& pos, const ImVec2& size, float scale);
  void DrawControls(float scale);
  // test: the live view (Controls > Test); else the bindings and sliders.
  void DrawController(float scale, bool test);
  void DrawKeyboard(float scale);
  // The first connected controller's raw state (SDL), false if none.
  bool ReadPad(rex::input::X_INPUT_GAMEPAD* out);
  void ClosePad();
  void SetPadSource(int target, int source);
  void DrawAchievements(float scale);
  void DrawSystem(float scale);  // Settings > System
  void DrawAbout(float scale);
  void DrawGameFolder(float label_w, float scale);
  bool SidebarButton(const char* label, bool selected, float width, float height);
  ImFont* HeadingFont() const;
  bool GameDataValid() const;
  void SetGameData(const std::string& path);
  void SetBind(const std::string& flag, const std::string& keys);
  void SaveSettings();

  rex::ui::ImmediateDrawer* immediate_drawer_;
  rex::ui::Window* window_;
  ImFont* font_;        // the game's MediumFont (text), or null for ImGui's
  ImFont* title_font_;  // the game's TitleFont (titles), or font_
  LauncherSettings settings_;
  std::filesystem::path settings_path_, user_data_root_;
  std::function<void(std::function<void()>)> call_in_ui_thread_deferred_;
  std::function<void()> on_relaunch_;
  std::function<bool(const LauncherSettings&)> in_game_needs_restart_;  // set in game only
  std::function<void(const LauncherSettings&, bool)> on_resume_;
  bool resume_requested_ = false;
  bool fonts_at_start_ = false;  // the game fonts were there when the launcher opened
  AssetJob asset_job_;
  std::vector<ProfileInfo> profiles_;
  bool saves_scanned_ = false;
  std::vector<EpisodeInfo> episodes_;  // from the game files, in settings_.language
  uint32_t episodes_language_ = 0;
  std::string confirm_;       // the delete button waiting for a second click
  std::string save_message_;  // result of the last save action
  Image logo_, panel_, title_, button_;
  Page page_ = Page::kSettings;
  std::vector<GpuInfo> gpus_;
  int automatic_gpu_ = -1;  // what vulkan_device=-1 picks
  std::vector<Achievement> achievements_;
  bool achievements_loaded_ = false;
  int achievement_filter_ = 0;  // 0 all, 1 unlocked, 2 locked
  char game_data_buffer_[1024] = {};
  char iso_target_buffer_[1024] = {};
  std::filesystem::path iso_path_;  // dropped / entered disc image waiting to be extracted
  std::string message_;             // shown above Play (validation errors)
  std::string capture_flag_;        // keybind being rebound
  int capture_slot_ = 0;            // 0 = primary key, 1 = alternate
  int controls_tab_ = 0;            // 0 test, 1 controller, 2 keyboard
  enum { kSettingsSystem, kSettingsDisplay, kSettingsShaders, kSettingsLogs, kSettingsEnhancements };
  int settings_tab_request_ = -1;   // Settings tab to open on the next frame
  bool link_hovered_ = false;       // About's repository link, last frame
  double copied_at_ = -10.0;        // About > Copy (ImGui time; -2 = failed)
  SDL_Gamepad* pad_ = nullptr;      // opened while Controls is shown
  bool pad_subsystem_ = false;      // SDL_INIT_GAMEPAD held by the launcher
  std::string pad_name_;
  int pad_capture_ = -1;            // controller button being rebound
  uint32_t pad_capture_held_ = 0;   // controls already down when it started
  ShaderBuild shader_build_;
  size_t shaders_not_in_cache_ = 0;
  double shaders_checked_at_ = -10.0;
  // Settings > Logs, newest first (read every 2 s while shown).
  struct LogEntry {
    std::filesystem::path path;
    uintmax_t size = 0;
    std::filesystem::file_time_type time;
  };
  std::vector<LogEntry> logs_;
  double logs_checked_at_ = -10.0;
  IsoExtract iso_extract_;
  // The game started by Play.
  struct GameProcess {
    int pid = -1;
    bool stopping = false;  // Stop was pressed
    std::chrono::steady_clock::time_point stop_time{};
    bool Start(std::string* error);
    void Stop();  // SIGTERM, then SIGKILL if it's still there after 5 s
    // Reaps it once it exits: false then (`failed` = it exited with an
    // error, not by Stop).
    bool Poll(bool* failed);
    bool Running() const { return pid > 0; }
  };
  GameProcess game_;
  std::filesystem::file_time_type settings_saved_time_{};  // after our last save
  bool playing_ = false;
  bool play_without_shaders_ = false;  // Play was pressed once without a shader cache
  // Frame limiter (the SDK repaints dialogs continuously).
  std::chrono::steady_clock::time_point last_frame_{}, last_input_{};
  void LimitFrameRate(ImGuiIO& io);
};

}  // namespace asura::ui
