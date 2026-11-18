#pragma once
// Launcher shown before the game starts (AsuraWrathApp::OnFinalizePaths), in
// a window: a sidebar (Controls, Achievements, Settings, Play) and a content
// panel. Settings are kept in <user data>/launcher.cfg and applied at startup
// as defaults, so command-line flags and the config TOML still win.
//
// Text uses the game's fonts when they were extracted (MediumFont.awf,
// TitleFont.awf; see ui/game_font.h).
// Optional art (found by GameUiFile, e.g. the repository's assets/ui):
//   launcher_logo.png   drawn at the top of the sidebar instead of the title
//   launcher_panel.png  stretched behind the content panel
// Skip it with --launcher=false (also skipped for scripted test runs).

#include <atomic>
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
#include <rex/ui/window_listener.h>

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
  bool keyboard = false;  // mnk_mode
  AspectMode aspect = AspectMode::kFit;
  int window_width = 0, window_height = 0;  // windowed size, 0 = default (1280x720)
  std::string game_data;                    // game_data_root, empty = flag / default
  std::map<std::string, std::string> binds;  // keybind_* flag -> keys

  static std::filesystem::path PathIn(const std::filesystem::path& user_data_root);
  bool Load(const std::filesystem::path& path);  // false if missing
  bool Save(const std::filesystem::path& path) const;
};

// Applies the settings to flags nothing else set (command line, environment,
// config TOML). Call before the GPU plugin is set up (OnPreSetup).
void ApplyLauncherDefaults(const LauncherSettings& settings);
// Opens the window (not fullscreen) for the launcher, unless the fullscreen
// flag was set elsewhere. Call after ApplyLauncherDefaults.
void UseWindowForLauncher();
// After Play: applies the window settings (size, fullscreen, aspect) live.
void ApplyWindowSettings(const LauncherSettings& settings);
// Settings that only apply when the game starts over (GPU, present mode,
// cropping).
bool NeedsRestart(const LauncherSettings& before, const LauncherSettings& after);

// mantra's shader cache location (MANTRA_SHADER_CACHE or the XDG cache).
std::filesystem::path ShaderCachePath();
// Where the game writes the achievement list and icons for the launcher
// (<XDG cache>/asura_wrath_recomp/achievements).
std::filesystem::path AchievementCacheDir();

class LauncherDialog : public rex::ui::ImGuiDialog,
                       public rex::ui::WindowListener,
                       public rex::ui::WindowInputListener {
 public:
  // `on_play` runs on the UI thread after the frame with the click; the
  // launcher is detached by then and may be destroyed by it.
  LauncherDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer,
                 rex::ui::Window* window, ImFont* font, ImFont* title_font,
                 LauncherSettings settings,
                 std::filesystem::path settings_path, std::filesystem::path user_data_root,
                 std::function<void(std::function<void()>)> call_in_ui_thread_deferred,
                 std::function<void(const LauncherSettings&)> on_play);
  ~LauncherDialog() override;

  // WindowListener / WindowInputListener.
  void OnFileDrop(rex::ui::FileDropEvent& e) override;
  void OnKeyDown(rex::ui::KeyEvent& e) override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class Page { kControls, kAchievements, kSettings };
  struct Image {
    std::unique_ptr<rex::ui::ImmediateTexture> texture;
    float width = 0, height = 0;
  };
  struct GpuInfo {
    std::string name;
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
    void Start(const std::filesystem::path& tool, const std::filesystem::path& game,
               const std::filesystem::path& out);
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

  Image LoadImage(const std::filesystem::path& path);
  void LoadAchievements();
  void UseDroppedPath(const std::filesystem::path& path);
  void DrawSidebar(const ImVec2& pos, const ImVec2& size, float scale);
  void DrawPanel(const ImVec2& pos, const ImVec2& size, float scale);
  void DrawControls(float scale);
  void DrawAchievements(float scale);
  void DrawSettings(float scale);
  void DrawGameFolder(float label_w, float scale);
  bool SidebarButton(const char* label, bool selected, float width, float height);
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
  std::function<void(const LauncherSettings&)> on_play_;
  Image logo_, panel_;
  Page page_ = Page::kSettings;
  std::vector<GpuInfo> gpus_;
  int automatic_gpu_ = -1;  // what vulkan_device=-1 picks
  std::vector<Achievement> achievements_;
  bool achievements_loaded_ = false;
  char game_data_buffer_[1024] = {};
  char iso_target_buffer_[1024] = {};
  std::filesystem::path iso_path_;  // dropped / entered disc image waiting to be extracted
  std::string message_;             // shown above Play (validation errors)
  std::string capture_flag_;        // keybind being rebound
  int capture_slot_ = 0;            // 0 = primary key, 1 = alternate
  ShaderBuild shader_build_;
  IsoExtract iso_extract_;
  bool playing_ = false;
};

}  // namespace asura::ui
