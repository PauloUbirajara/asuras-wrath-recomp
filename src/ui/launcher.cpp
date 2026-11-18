#include "launcher.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/disc_image_entry.h>
#include <rex/filesystem/entry.h>
#include <rex/logging.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/window.h>

#include <SDL3/SDL.h>

#include "../game/frame_rate.h"
#include "../input/pad_remap.h"
#include "../mantra_renderer/shader_cache_format.h"
#include "game_assets.h"
#include "game_font.h"
#include "game_toast.h"
#include "launcher_text.h"

extern "C" unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x,
                                                 int* y, int* channels_in_file,
                                                 int desired_channels);
extern "C" void stbi_image_free(void* retval_from_stbi_load);

extern char** environ;

REXCVAR_DEFINE_BOOL(launcher, true, "Asura", "Show the launcher before the game starts");

namespace asura::ui {

namespace {

constexpr uint32_t kTitleId = 0x43430817;

// Keyboard bindings shown in Controls, in the game's button order.
struct BindRow {
  const char* label;
  const char* flag;
};
constexpr BindRow kBindRows[] = {
    {"A", "keybind_a"},
    {"B", "keybind_b"},
    {"X", "keybind_x"},
    {"Y", "keybind_y"},
    {"Left trigger", "keybind_left_trigger"},
    {"Right trigger", "keybind_right_trigger"},
    {"Left shoulder", "keybind_left_shoulder"},
    {"Right shoulder", "keybind_right_shoulder"},
    {"Left stick up", "keybind_lstick_up"},
    {"Left stick down", "keybind_lstick_down"},
    {"Left stick left", "keybind_lstick_left"},
    {"Left stick right", "keybind_lstick_right"},
    {"Left stick press", "keybind_lstick_press"},
    {"Right stick up", "keybind_rstick_up"},
    {"Right stick down", "keybind_rstick_down"},
    {"Right stick left", "keybind_rstick_left"},
    {"Right stick right", "keybind_rstick_right"},
    {"Right stick press", "keybind_rstick_press"},
    {"D-pad up", "keybind_dpad_up"},
    {"D-pad down", "keybind_dpad_down"},
    {"D-pad left", "keybind_dpad_left"},
    {"D-pad right", "keybind_dpad_right"},
    {"Start", "keybind_start"},
    {"Back", "keybind_back"},
};

// The SDK's overlay keys (rex_app.cpp RegisterBind): one key each, no
// modifiers, active whether or not the keyboard plays.
constexpr BindRow kOverlayRows[] = {
    {"Performance overlay", "bind_debug_overlay"},
    {"Settings overlay", "bind_settings"},
    {"Achievements overlay", "bind_achievements"},
    {"Console", "bind_console"},
    {"Launcher (in game)", "bind_launcher"},
};

bool IsOverlayBind(std::string_view flag) { return flag.rfind("bind_", 0) == 0; }

struct ScreenSize {
  int width, height;
};

// "1920 x 1080 (16:9)": the usual name of the shape, else the reduced ratio.
std::string SizeLabel(int width, int height) {
  struct Named {
    int w, h;
    const char* name;
  };
  static const Named kNames[] = {{4, 3, "4:3"},   {5, 4, "5:4"},     {3, 2, "3:2"},
                                 {16, 10, "16:10"}, {16, 9, "16:9"}, {64, 27, "21:9"},
                                 {43, 18, "21:9"}, {12, 5, "21:9"},  {32, 9, "32:9"}};
  std::string ratio;
  const double r = double(width) / double(std::max(height, 1));
  for (const auto& n : kNames) {
    if (std::abs(r - double(n.w) / n.h) < 0.012 * r) {
      ratio = n.name;
      break;
    }
  }
  if (ratio.empty()) {
    const int g = std::gcd(width, std::max(height, 1));
    ratio = std::to_string(width / g) + ":" + std::to_string(height / g);
  }
  return std::to_string(width) + " x " + std::to_string(height) + " (" + ratio + ")";
}

// The resolutions a monitor (1-based, 0 = primary) offers, largest first,
// plus the common small window sizes that fit on it.
std::vector<ScreenSize> MonitorSizes(int monitor) {
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  const SDL_DisplayID display =
      monitor > 0 && monitor <= count ? displays[monitor - 1] : SDL_GetPrimaryDisplay();
  SDL_free(displays);
  std::vector<ScreenSize> sizes;
  int mode_count = 0;
  if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &mode_count)) {
    for (int i = 0; i < mode_count; ++i) sizes.push_back({modes[i]->w, modes[i]->h});
    SDL_free(modes);
  }
  int max_w = 0, max_h = 0;
  for (const auto& s : sizes) {
    max_w = std::max(max_w, s.width);
    max_h = std::max(max_h, s.height);
  }
  if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display)) {
    sizes.push_back({desktop->w, desktop->h});
    max_w = std::max(max_w, desktop->w);
    max_h = std::max(max_h, desktop->h);
  }
  static const ScreenSize kCommon[] = {{640, 480},   {800, 600},   {1024, 768},  {1280, 720},
                                       {1280, 800},  {1600, 900},  {1920, 1080}, {2560, 1080},
                                       {2560, 1440}, {3440, 1440}, {3840, 2160}};
  for (const auto& s : kCommon) {
    if (!max_w || (s.width <= max_w && s.height <= max_h)) sizes.push_back(s);
  }
  std::sort(sizes.begin(), sizes.end(), [](const ScreenSize& a, const ScreenSize& b) {
    return a.width != b.width ? a.width > b.width : a.height > b.height;
  });
  sizes.erase(std::unique(sizes.begin(), sizes.end(),
                          [](const ScreenSize& a, const ScreenSize& b) {
                            return a.width == b.width && a.height == b.height;
                          }),
              sizes.end());
  return sizes;
}

const char* AspectName(AspectMode mode) {
  switch (mode) {
    case AspectMode::kFill: return "fill";
    case AspectMode::kStretch: return "stretch";
    default: return "fit";
  }
}

// Whether something other than the defaults (command line, environment,
// config TOML) set the flag; the launcher leaves those alone.
bool SetElsewhere(std::string_view name) {
  const auto source = rex::cvar::GetFlagSource(name);
  return source != rex::cvar::Source::kDefault && source != rex::cvar::Source::kRuntime;
}

// Sets a flag nothing else set, keeping it a default (so the config TOML is
// not written with it and a later command line still wins).
void SetIfUnset(std::string_view name, std::string_view value) {
  for (auto& entry : rex::cvar::GetRegistry()) {
    if (entry.name != name) continue;
    if (entry.source == rex::cvar::Source::kDefault && entry.setter) entry.setter(value);
    return;
  }
}

// Sets a flag through the registry (runs change callbacks, so the window
// follows), unless something else set it.
void SetLive(std::string_view name, std::string_view value) {
  if (SetElsewhere(name) || rex::cvar::GetFlagByName(name) == value) return;
  rex::cvar::SetFlagByName(name, value);
}

std::string FlagValue(std::string_view name) { return rex::cvar::GetFlagByName(name); }

std::string FlagDefault(std::string_view name) {
  const auto* info = rex::cvar::GetFlagInfo(name);
  return info ? info->default_value : std::string();
}


std::vector<std::string> SplitKeys(const std::string& keys) {
  std::vector<std::string> out;
  std::stringstream in(keys);
  std::string token;
  while (std::getline(in, token, ',')) {
    const size_t a = token.find_first_not_of(' '), b = token.find_last_not_of(' ');
    out.push_back(a == std::string::npos ? std::string() : token.substr(a, b - a + 1));
  }
  return out;
}

std::string JoinKeys(const std::vector<std::string>& keys) {
  std::string out;
  for (const auto& k : keys) {
    if (k.empty()) continue;
    if (!out.empty()) out += ',';
    out += k;
  }
  return out;
}

bool IsModifierKey(rex::ui::VirtualKey vk) {
  using rex::ui::VirtualKey;
  switch (vk) {
    case VirtualKey::kShift:
    case VirtualKey::kControl:
    case VirtualKey::kMenu:
    case VirtualKey::kLShift:
    case VirtualKey::kRShift:
    case VirtualKey::kLControl:
    case VirtualKey::kRControl:
    case VirtualKey::kLMenu:
    case VirtualKey::kRMenu:
    case VirtualKey::kLWin:
    case VirtualKey::kRWin:
      return true;
    default:
      return false;
  }
}

std::string FormatBytes(uint64_t bytes) {
  char buffer[32];
  if (bytes >= (1ull << 30)) {
    std::snprintf(buffer, sizeof(buffer), "%.1f GB", double(bytes) / double(1ull << 30));
  } else if (bytes < (1ull << 20)) {
    std::snprintf(buffer, sizeof(buffer), "%.0f KB", std::ceil(double(bytes) / 1024.0));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.0f MB", double(bytes) / double(1ull << 20));
  }
  return buffer;
}

bool IsDiscImage(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return false;
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
  return ext == ".iso" || ext == ".xiso";
}

}  // namespace

// --- Settings ----------------------------------------------------------------

std::filesystem::path LauncherSettings::PathIn(const std::filesystem::path& user_data_root) {
  return user_data_root / "launcher.cfg";
}

bool LauncherSettings::Load(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#') continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    const bool on = value == "1" || value == "true";
    if (key == "gpu") gpu = std::atoi(value.c_str());
    else if (key == "vsync") vsync = on;
    else if (key == "fullscreen") fullscreen = on;
    else if (key == "exclusive") exclusive = on;
    else if (key == "monitor") monitor = std::clamp(std::atoi(value.c_str()), 0, 16);
    else if (key == "keyboard") keyboard = on;
    else if (key == "aspect")
      aspect = value == "fill" ? AspectMode::kFill
               : value == "stretch" ? AspectMode::kStretch
                                    : AspectMode::kFit;
    else if (key == "window") std::sscanf(value.c_str(), "%dx%d", &window_width, &window_height);
    else if (key == "game_data") game_data = value;
    else if (key == "language") language = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
    else if (key == "achievements_view") achievements_grid = value == "grid";
    else if (key == "deadzone_left") deadzone_left = std::strtod(value.c_str(), nullptr);
    else if (key == "deadzone_right") deadzone_right = std::strtod(value.c_str(), nullptr);
    else if (key == "vibration") vibration = std::clamp(std::atoi(value.c_str()), 0, 200);
    else if (key == "show_fps") show_fps = on;
    else if (key == "hide_cursor") hide_cursor = on;
    else if (key == "mute_background") mute_background = on;
    else if (key == "turbo") {
      turbo = 0;
      std::stringstream names(value);
      for (std::string name; std::getline(names, name, ',');) {
        const int control = input::PadControlFromName(name);
        if (control >= 0) turbo |= 1u << control;
      }
    }
    else if (key == "native_render") native_render = on;
    else if (key == "fps_cap") fps_cap = std::clamp(std::atoi(value.c_str()), 0, 240);
    else if (key == "render_height") render_height = std::clamp(std::atoi(value.c_str()), 360, 2880);
    else if (key == "toasts") toasts = on;
    else if (key == "toast_corner") toast_corner = std::clamp(std::atoi(value.c_str()), 0, 3);
    else if (key == "toast_seconds")
      toast_seconds = std::clamp(std::strtod(value.c_str(), nullptr), 1.0, 15.0);
    else if (key.rfind("pad.", 0) == 0) pad[key.substr(4)] = value;
    else if (key.rfind("flag.", 0) == 0) binds[key.substr(5)] = value;
  }
  return true;
}

bool LauncherSettings::Save(const std::filesystem::path& path) const {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if (!out) return false;
  out << "# Asura's Wrath Recompiled launcher settings\n"
      << "gpu=" << gpu << "\nvsync=" << vsync << "\nfullscreen=" << fullscreen << "\nexclusive=" << exclusive
      << "\nmonitor=" << monitor
      << "\nkeyboard=" << keyboard << "\naspect=" << AspectName(aspect) << "\nwindow="
      << window_width << "x" << window_height << "\nlanguage=" << language
      << "\ngame_data=" << game_data << "\ndeadzone_left=" << deadzone_left
      << "\ndeadzone_right=" << deadzone_right << "\nvibration=" << vibration
      << "\nshow_fps=" << show_fps << "\nfps_cap=" << fps_cap << "\nrender_height=" << render_height << "\nnative_render=" << native_render << "\ntoasts=" << toasts << "\ntoast_corner=" << toast_corner
      << "\ntoast_seconds=" << toast_seconds << "\nhide_cursor=" << hide_cursor
      << "\nmute_background=" << mute_background << "\nturbo=";
  for (int c = 0, n = 0; c < input::kPadControlCount; ++c) {
    if (turbo & (1u << c)) out << (n++ ? "," : "") << input::PadControlName(c);
  }
  out
      << "\nachievements_view=" << (achievements_grid ? "grid" : "list") << "\n";
  for (const auto& [target, source] : pad) out << "pad." << target << "=" << source << "\n";
  for (const auto& [flag, value] : binds) out << "flag." << flag << "=" << value << "\n";
  return bool(out);
}

// MANTRA_RENDER_SCALE set before the launcher's settings were first applied
// (from the shell): it wins over Render resolution.
bool RenderScaleFromEnvironment() {
  static const bool from_environment = std::getenv("MANTRA_RENDER_SCALE") != nullptr;
  return from_environment;
}

void ApplyLauncherDefaults(const LauncherSettings& s) {
  SetIfUnset("vulkan_device", std::to_string(s.gpu));
  SetIfUnset("vulkan_allow_present_mode_immediate", s.vsync ? "false" : "true");
  SetIfUnset("vulkan_allow_present_mode_mailbox", s.vsync ? "false" : "true");
  SetIfUnset("fullscreen", s.fullscreen ? "true" : "false");
  SetIfUnset("fullscreen_exclusive", s.exclusive ? "true" : "false");
  SetIfUnset("monitor", std::to_string(s.monitor));
  SetIfUnset("mnk_mode", s.keyboard ? "true" : "false");
  if (s.language) SetIfUnset("user_language", std::to_string(s.language));
  if (s.window_width > 0 && s.window_height > 0) {
    SetIfUnset("window_width", std::to_string(s.window_width));
    SetIfUnset("window_height", std::to_string(s.window_height));
  }
  // Fit: 16:9 with black bars. Stretch: fill, distorted. Fill: let the
  // presenter crop the top and bottom (or sides) as far as needed.
  SetIfUnset("present_letterbox", s.aspect == AspectMode::kStretch ? "false" : "true");
  const bool crop = s.aspect == AspectMode::kFill;
  SetIfUnset("present_allow_overscan_cutoff", crop ? "true" : "false");
  if (crop) {
    SetIfUnset("present_safe_area_x", "1");
    SetIfUnset("present_safe_area_y", "1");
  }
  for (const auto& [flag, value] : s.binds) SetIfUnset(flag, value);
  SetIfUnset("left_stick_deadzone_percentage", std::to_string(s.deadzone_left));
  SetIfUnset("right_stick_deadzone_percentage", std::to_string(s.deadzone_right));
  input::SetPadMap(PadMapFrom(s));
  input::SetVibrationStrength(s.vibration);
  input::SetTurboControls(s.turbo);
  game::SetFrameRateCap(s.fps_cap);
  // mantra reads it when it starts (vk_image.h RenderScale8).
  // Unless the environment set it before the first call: the launcher's
  // process applies its start-up settings, and the game process (started by
  // Play) inherits the environment, so a stale value must not stay.
  if (!RenderScaleFromEnvironment()) {
    if (RenderHeight(s) != 720) {
      setenv("MANTRA_RENDER_SCALE", std::to_string(RenderHeight(s)).c_str(), 1);
    } else {
      unsetenv("MANTRA_RENDER_SCALE");
    }
  }
  SetToastOptions({s.toasts, s.toast_corner, float(s.toast_seconds)});
}

input::PadMap PadMapFrom(const LauncherSettings& s) {
  input::PadMap map = input::IdentityPadMap();
  for (const auto& [target, source] : s.pad) {
    const int t = input::PadControlFromName(target), from = input::PadControlFromName(source);
    if (t >= 0 && from >= 0) map[t] = uint8_t(from);
  }
  return map;
}

void UseWindowForLauncher() { SetIfUnset("fullscreen", "false"); }

void ApplyWindowSettings(const LauncherSettings& s) {
  if (s.window_width > 0 && s.window_height > 0) {
    SetLive("window_width", std::to_string(s.window_width));
    SetLive("window_height", std::to_string(s.window_height));
  }
  SetLive("present_letterbox", s.aspect == AspectMode::kStretch ? "false" : "true");
  SetLive("fullscreen_exclusive", s.exclusive ? "true" : "false");
  SetLive("fullscreen", s.fullscreen ? "true" : "false");
}

void ApplyLiveSettings(const LauncherSettings& s) {
  SetLive("mnk_mode", s.keyboard ? "true" : "false");
  for (const auto& [flag, value] : s.binds) SetLive(flag, value);
  SetLive("left_stick_deadzone_percentage", std::to_string(s.deadzone_left));
  SetLive("right_stick_deadzone_percentage", std::to_string(s.deadzone_right));
  input::SetPadMap(PadMapFrom(s));
  input::SetVibrationStrength(s.vibration);
  input::SetTurboControls(s.turbo);
  game::SetFrameRateCap(s.fps_cap);
  SetToastOptions({s.toasts, s.toast_corner, float(s.toast_seconds)});
}

bool NeedsRestart(const LauncherSettings& before, const LauncherSettings& after) {
  return before.gpu != after.gpu || before.vsync != after.vsync ||
         RenderHeight(before) != RenderHeight(after) || before.monitor != after.monitor ||
         (before.fullscreen && before.exclusive) != (after.fullscreen && after.exclusive) ||
         (before.aspect == AspectMode::kFill) != (after.aspect == AspectMode::kFill);
}

void ApplyOrangeAccent(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
  // Same saturation and brightness, hue moved from green to the launcher's
  // orange (kAccent, about 15 degrees).
  auto orange = [](ImVec4& c) {
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, s, v);
    if (s < 0.2f || h < 0.17f || h > 0.50f) return;  // not green
    ImGui::ColorConvertHSVtoRGB(0.04f, s, v, c.x, c.y, c.z);
  };
  for (ImVec4& c : imgui_style.Colors) orange(c);
  auto& a = ui_style.achievements;
  for (ImVec4* c : {&a.unlocked_title, &a.unlocked_desc, &a.row_unlocked_bg, &a.progress_bar}) {
    orange(*c);
  }
  // The settings' lifecycle badges keep their traffic-light colors.
}

std::filesystem::path ShaderCachePath() {
  if (const char* env = std::getenv("MANTRA_SHADER_CACHE"); env && *env) return env;
  const auto root = GameCacheRoot();
  return root.empty() ? root : root / "mantra_shaders.mshc";
}

std::vector<std::filesystem::path> ShaderDumpDirs() {
  std::vector<std::filesystem::path> dirs;
  const auto cache = ShaderCachePath();
  if (!cache.empty()) dirs.push_back(cache.parent_path() / "shader_dumps");
  // Development: the repository's dumps (MANTRA_SHADER_DUMP=out/mantra_dumps),
  // from out/build/<preset>/.
  std::error_code ec;
  const auto repo = rex::filesystem::GetExecutableFolder().parent_path().parent_path() /
                    "mantra_dumps";
  if (std::filesystem::is_directory(repo, ec)) dirs.push_back(repo);
  return dirs;
}

size_t ShadersNotInCache() {
  std::error_code ec;
  const auto cache = ShaderCachePath();
  const auto built = std::filesystem::last_write_time(cache, ec);
  if (ec) return 0;
  size_t count = 0;
  for (const auto& dir : ShaderDumpDirs()) {
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      if (entry.path().extension() == ".mc" && entry.last_write_time(ec) > built) ++count;
    }
  }
  return count;
}

// The cache file's format version, 0 if missing or not a cache (read again
// when the file changes).
uint32_t ShaderCacheVersion() {
  namespace sc = mantra::shader_cache;
  static std::filesystem::path path;
  static std::filesystem::file_time_type time;
  static uint32_t version = 0;
  const auto cache = ShaderCachePath();
  std::error_code ec;
  const auto written = std::filesystem::last_write_time(cache, ec);
  if (ec) return 0;
  if (cache != path || written != time) {
    path = cache;
    time = written;
    version = 0;
    sc::CacheHeader header{};
    std::ifstream in(cache, std::ios::binary);
    if (in.read(reinterpret_cast<char*>(&header), sizeof(header)) &&
        std::memcmp(header.magic, sc::kMagic, 4) == 0) {
      version = header.version;
    }
  }
  return version;
}

// Whether the cache works with these settings. Shaders don't depend on the
// settings themselves; only a render resolution other than the game's needs
// a cache from version 8 on (older ones misplace screen-space effects).
ShaderCacheFit ShaderCacheFitFor(const LauncherSettings& s) {
  namespace sc = mantra::shader_cache;
  const uint32_t version = ShaderCacheVersion();
  if (version < sc::kMinVersion || version > sc::kVersion) return ShaderCacheFit::kMissing;
  const uint32_t needed = RenderHeight(s) != 720 ? 8 : sc::kMinVersion;
  return version < needed ? ShaderCacheFit::kOutdated : ShaderCacheFit::kReady;
}

int RenderHeight(const LauncherSettings& s) { return s.native_render ? s.render_height : 720; }

bool HaveGameFonts() { return HaveExtractedUiFonts(); }

// --- Shader cache build --------------------------------------------------------

void LauncherDialog::ShaderBuild::Start(const std::filesystem::path& tool,
                                        const std::filesystem::path& game,
                                        const std::filesystem::path& out,
                                        const std::vector<std::filesystem::path>& dumps) {
  Stop();
  std::error_code ec;
  std::filesystem::create_directories(out.parent_path(), ec);
  int fds[2];
  if (pipe(fds) != 0) {
    state = kFailed;
    return;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  const std::string tool_s = tool.string(), game_s = game.string(), out_s = out.string();
  std::vector<std::string> dump_s;
  for (const auto& dir : dumps) dump_s.push_back(dir.string());
  std::vector<const char*> argv = {tool_s.c_str(), "--game", game_s.c_str(), "--out", out_s.c_str()};
  for (const auto& dir : dump_s) {
    argv.push_back("--dumps");
    argv.push_back(dir.c_str());
  }
  argv.push_back(nullptr);
  pid_t child = -1;
  const int rc = posix_spawn(&child, tool_s.c_str(), &actions, nullptr,
                             const_cast<char* const*>(argv.data()), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    std::lock_guard<std::mutex> lock(mutex);
    last_line = T("Could not start ") + tool_s + ": " + std::strerror(rc);
    state = kFailed;
    return;
  }
  pid = child;
  state = kRunning;
  {
    std::lock_guard<std::mutex> lock(mutex);
    last_line = T("Starting...");
  }
  done = 0;
  total = 0;
  reader = std::thread([this, fd = fds[0], child] {
    std::string line;
    char buffer[4096];
    for (;;) {
      const ssize_t n = read(fd, buffer, sizeof(buffer));
      if (n <= 0) break;
      for (ssize_t i = 0; i < n; ++i) {
        const char c = buffer[i];
        if (c == '\n' || c == '\r') {
          if (!line.empty()) {
            unsigned n = 0, m = 0;
            if (std::sscanf(line.c_str(), " translated %u/%u", &n, &m) == 2 && m) {
              done = int(n);
              total = int(m);
            }
            std::lock_guard<std::mutex> lock(mutex);
            last_line = line;
          }
          line.clear();
        } else {
          line += c;
        }
      }
    }
    close(fd);
    int status = 0;
    waitpid(child, &status, 0);
    state = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? kDone : kFailed;
  });
}

void LauncherDialog::ShaderBuild::Stop() {
  if (state == kRunning && pid > 0) kill(pid, SIGTERM);
  if (reader.joinable()) reader.join();
  pid = -1;
}

std::string LauncherDialog::ShaderBuild::Line() {
  std::lock_guard<std::mutex> lock(mutex);
  return last_line;
}

// --- Disc image extraction -------------------------------------------------------

void LauncherDialog::IsoExtract::Start(const std::filesystem::path& iso,
                                       const std::filesystem::path& out_dir) {
  Stop();
  cancel = false;
  done_bytes = 0;
  total_bytes = 0;
  out = out_dir;
  state = kRunning;
  {
    std::lock_guard<std::mutex> lock(mutex);
    message = T("Reading the disc image...");
  }
  worker = std::thread([this, iso, out_dir] {
    auto fail = [this](std::string text) {
      std::lock_guard<std::mutex> lock(mutex);
      message = std::move(text);
      state = kFailed;
    };
    rex::filesystem::DiscImageDevice device("\\Device\\LauncherDisc", iso);
    if (!device.Initialize() || !device.root()) {
      fail(T("Not an Xbox 360 game disc image: ") + iso.filename().string());
      return;
    }
    total_bytes = device.total_file_size();
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
      fail(T("Cannot create ") + out_dir.string() + ": " + ec.message());
      return;
    }
    const auto space = std::filesystem::space(out_dir, ec);
    if (!ec && space.available < total_bytes) {
      char text[160];
      std::snprintf(text, sizeof(text), T("Not enough free space: %s needed, %s free"),
                    FormatBytes(total_bytes).c_str(), FormatBytes(space.available).c_str());
      fail(text);
      return;
    }
    std::string error;
    std::function<bool(const rex::filesystem::Entry&, const std::filesystem::path&)> copy =
        [&](const rex::filesystem::Entry& dir, const std::filesystem::path& to) {
          for (const auto& child : dir.children()) {
            if (cancel) return false;
            const std::filesystem::path dest = to / child->name();
            if (child->attributes() & rex::filesystem::kFileAttributeDirectory) {
              std::filesystem::create_directories(dest, ec);
              if (!copy(*child, dest)) return false;
              continue;
            }
            {
              std::lock_guard<std::mutex> lock(mutex);
              message = child->path();
            }
            const auto& entry = static_cast<const rex::filesystem::DiscImageEntry&>(*child);
            const uint8_t* data = entry.mmap()->data() + entry.data_offset();
            std::ofstream file(dest, std::ios::binary | std::ios::trunc);
            constexpr size_t kChunk = 8u << 20;
            for (size_t offset = 0; file && offset < entry.data_size(); offset += kChunk) {
              if (cancel) return false;
              const size_t n = std::min(kChunk, entry.data_size() - offset);
              file.write(reinterpret_cast<const char*>(data + offset), std::streamsize(n));
              done_bytes += n;
            }
            if (!file) {
              error = T("Write failed: ") + dest.string();
              return false;
            }
          }
          return true;
        };
    if (!copy(*device.root(), out_dir)) {
      fail(cancel ? std::string(T("Cancelled")) : error);
      return;
    }
    if (!std::filesystem::is_regular_file(out_dir / "default.xex", ec)) {
      fail(T("The disc image has no default.xex"));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      message = T("Done");
    }
    state = kDone;
  });
}

void LauncherDialog::IsoExtract::Stop() {
  cancel = true;
  if (worker.joinable()) worker.join();
}

std::string LauncherDialog::IsoExtract::Message() {
  std::lock_guard<std::mutex> lock(mutex);
  return message;
}

// --- Dialog ----------------------------------------------------------------------

namespace {
constexpr ImVec4 kBackground(0.118f, 0.118f, 0.122f, 1.0f);
constexpr ImVec4 kSidebar(0.098f, 0.098f, 0.106f, 1.0f);
constexpr ImVec4 kPanel(0.60f, 0.56f, 0.50f, 1.0f);
constexpr ImVec4 kPanelBand(0.47f, 0.42f, 0.36f, 1.0f);
constexpr ImVec4 kPanelText(0.13f, 0.10f, 0.08f, 1.0f);
constexpr ImVec4 kPanelTextDim(0.27f, 0.23f, 0.19f, 1.0f);
constexpr ImVec4 kField(0.76f, 0.72f, 0.65f, 1.0f);
constexpr ImVec4 kFieldHover(0.82f, 0.78f, 0.71f, 1.0f);
constexpr ImVec4 kButton(0.20f, 0.20f, 0.21f, 1.0f);
constexpr ImVec4 kButtonHover(0.28f, 0.28f, 0.30f, 1.0f);
constexpr ImVec4 kButtonSelected(0.36f, 0.30f, 0.24f, 1.0f);
constexpr ImVec4 kAccent(0.80f, 0.33f, 0.18f, 1.0f);
constexpr ImVec4 kUnlockedRow(1.0f, 0.97f, 0.88f, 0.22f);

ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
}  // namespace

LauncherDialog::LauncherDialog(rex::ui::ImGuiDrawer* drawer,
                               rex::ui::ImmediateDrawer* immediate_drawer, rex::ui::Window* window,
                               ImFont* font, ImFont* title_font, LauncherSettings settings,
                               std::filesystem::path settings_path,
                               std::filesystem::path user_data_root,
                               std::function<void(std::function<void()>)> call_in_ui_thread_deferred,
                               std::function<void()> on_relaunch)
    : rex::ui::ImGuiDialog(drawer),
      immediate_drawer_(immediate_drawer),
      window_(window),
      font_(font),
      title_font_(title_font ? title_font : font),
      settings_(std::move(settings)),
      settings_path_(std::move(settings_path)),
      user_data_root_(std::move(user_data_root)),
      call_in_ui_thread_deferred_(std::move(call_in_ui_thread_deferred)),
      on_relaunch_(std::move(on_relaunch)) {
  fonts_at_start_ = HaveGameFonts();
  SetLauncherLanguage(settings_.language);
  logo_ = LoadImage(GameUiFile("logo.png"));
  if (!logo_.texture) logo_ = LoadImage(GameUiFile("launcher_logo.png"));
  panel_ = LoadImage(GameUiFile("launcher_background.png"));
  button_ = LoadImage(GameUiFile("menu_button.png"), true);
  title_ = LoadImage(GameUiFile("launcher_title.png"));
  std::snprintf(game_data_buffer_, sizeof(game_data_buffer_), "%s", settings_.game_data.c_str());
  // GPUs in the order the SDK numbers them (vulkan_device), and the one it
  // picks on its own: the first with the most of the preferred features
  // (VulkanProvider::Create).
  if (auto instance = rex::ui::vulkan::VulkanInstance::Create(false, false)) {
    std::vector<VkPhysicalDevice> devices;
    instance->EnumeratePhysicalDevices(devices);
    const auto& ifn = instance->functions();
    const bool want_gs = FlagValue("vulkan_prefer_geometry_shader") == "true";
    const bool want_fs = FlagValue("vulkan_prefer_fragment_stores_and_atomics") == "true";
    const bool want_vs = FlagValue("vulkan_prefer_vertex_pipeline_stores_and_atomics") == "true";
    const bool want_fill = FlagValue("vulkan_prefer_fill_mode_non_solid") == "true";
    int best_score = -1;
    for (VkPhysicalDevice device : devices) {
      VkPhysicalDeviceProperties properties;
      ifn.vkGetPhysicalDeviceProperties(device, &properties);
      VkPhysicalDeviceFeatures features = {};
      ifn.vkGetPhysicalDeviceFeatures(device, &features);
      const int score = (want_gs && features.geometryShader) +
                        (want_fs && features.fragmentStoresAndAtomics) +
                        (want_vs && features.vertexPipelineStoresAndAtomics) +
                        (want_fill && features.fillModeNonSolid);
      if (score > best_score) {
        best_score = score;
        automatic_gpu_ = int(gpus_.size());
      }
      gpus_.push_back({properties.deviceName});
    }
  }
  if (window_) {
    window_->AddListener(this);
    // Above the ImGui drawer (64) so key presses for rebinding come here first.
    window_->AddInputListener(this, 1000);
  }
  if (!GameDataValid()) page_ = Page::kSettings;
  StartAssetJob();
}

void LauncherDialog::SetInGame(std::function<bool(const LauncherSettings&)> needs_restart,
                               std::function<void(const LauncherSettings&, bool)> on_resume) {
  in_game_needs_restart_ = std::move(needs_restart);
  on_resume_ = std::move(on_resume);
}

LauncherDialog::~LauncherDialog() {
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  shader_build_.Stop();
  iso_extract_.Stop();
  asset_job_.Join();
  ClosePad();
  if (pad_subsystem_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

LauncherDialog::Image LauncherDialog::LoadImage(const std::filesystem::path& path, bool repeat) {
  Image image;
  std::ifstream file(path, std::ios::binary);
  if (!file || !immediate_drawer_) return image;
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
  int w = 0, h = 0, channels = 0;
  unsigned char* rgba = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
  if (!rgba) return image;
  image.texture = immediate_drawer_->CreateTexture(
      uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, repeat, rgba);
  // Bounds of the visible pixels, so art with transparent margins (a logo)
  // can be laid out by what shows.
  int x0 = w, y0 = h, x1 = -1, y1 = -1;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (rgba[(size_t(y) * w + x) * 4 + 3] < 8) continue;
      x0 = std::min(x0, x);
      x1 = std::max(x1, x);
      y0 = std::min(y0, y);
      y1 = std::max(y1, y);
    }
  }
  stbi_image_free(rgba);
  image.width = float(w);
  image.height = float(h);
  if (x1 >= x0 && y1 >= y0) {
    image.uv0 = ImVec2(float(x0) / w, float(y0) / h);
    image.uv1 = ImVec2(float(x1 + 1) / w, float(y1 + 1) / h);
  }
  return image;
}

// The list read from default.xex (game_assets.h), with the unlocks the SDK
// saves in <user data>/achievements/<title id>.toml.
void LauncherDialog::LoadAchievements() {
  achievements_loaded_ = true;
  achievements_.clear();
  const auto dir = AchievementCacheDir();
  std::ifstream tsv(dir / "achievements.tsv");
  if (!tsv) return;
  std::string line;
  std::vector<std::string> header;
  auto split = [](const std::string& s) {
    std::vector<std::string> out;
    std::stringstream in(s);
    std::string field;
    while (std::getline(in, field, '\t')) out.push_back(field);
    return out;
  };
  if (std::getline(tsv, line)) header = split(line);
  auto column = [&](const char* name) {
    const auto it = std::find(header.begin(), header.end(), name);
    return it == header.end() ? -1 : int(it - header.begin());
  };
  const int c_id = column("id"), c_gs = column("gamerscore"), c_name = column("name"),
            c_desc = column("description"), c_locked = column("locked_description");
  while (std::getline(tsv, line)) {
    const auto f = split(line);
    auto get = [&](int c) { return c >= 0 && size_t(c) < f.size() ? f[c] : std::string(); };
    Achievement a;
    a.id = uint32_t(std::strtoul(get(c_id).c_str(), nullptr, 10));
    if (!a.id) continue;
    a.gamerscore = uint32_t(std::strtoul(get(c_gs).c_str(), nullptr, 10));
    a.name = get(c_name);
    a.description = get(c_desc);
    a.locked_description = get(c_locked);
    // The XDBF's locked texts are "<String Placeholder>" for most entries.
    if (a.locked_description.empty() || a.locked_description[0] == '<') {
      a.locked_description = a.description;
    }
    a.icon = LoadImage(dir / (std::to_string(a.id) + ".png"));
    achievements_.push_back(std::move(a));
  }
  // [unlocked.<id>] / filetime = <FILETIME>
  char unlock_name[32];
  std::snprintf(unlock_name, sizeof(unlock_name), "%08X.toml", kTitleId);
  std::ifstream unlocks(user_data_root_ / "achievements" / unlock_name);
  Achievement* current = nullptr;
  while (unlocks && std::getline(unlocks, line)) {
    uint32_t id = 0;
    unsigned long long filetime = 0;
    if (std::sscanf(line.c_str(), "[unlocked.%u]", &id) == 1) {
      current = nullptr;
      for (auto& a : achievements_) {
        if (a.id == id) current = &a;
      }
      if (current) current->unlocked_filetime = 1;
    } else if (current && std::sscanf(line.c_str(), "filetime = %llu", &filetime) == 1) {
      current->unlocked_filetime = filetime ? filetime : 1;
    }
  }
  // Most recent unlocks first (unknown dates after them), then the locked ones
  // in the game's order.
  std::stable_sort(achievements_.begin(), achievements_.end(),
                   [](const Achievement& a, const Achievement& b) {
                     return a.unlocked_filetime > b.unlocked_filetime;
                   });
}

bool LauncherDialog::GameDataValid() const {
  std::error_code ec;
  const std::filesystem::path root = settings_.game_data;
  return !root.empty() && std::filesystem::is_regular_file(root / "default.xex", ec);
}

void LauncherDialog::SetGameData(const std::string& path) {
  settings_.game_data = path;
  std::snprintf(game_data_buffer_, sizeof(game_data_buffer_), "%s", path.c_str());
  message_.clear();
  SaveSettings();
  StartAssetJob();
}

void LauncherDialog::SetBind(const std::string& flag, const std::string& keys) {
  if (keys == FlagDefault(flag)) {
    settings_.binds.erase(flag);
  } else {
    settings_.binds[flag] = keys;
  }
  SetIfUnset(flag, keys);
  SaveSettings();
}

void LauncherDialog::SaveSettings() {
  if (!settings_.Save(settings_path_)) {
    REXLOG_WARN("launcher: cannot write {}", settings_path_.string());
  }
  std::error_code ec;
  settings_saved_time_ = std::filesystem::last_write_time(settings_path_, ec);
}

// This executable with its arguments, --launcher=false instead of any
// --launcher flag, and its own log file when one was given (both processes
// would write the same file otherwise): run.log -> run-game.log.
bool LauncherDialog::GameProcess::Start(std::string* error) {
  std::vector<std::string> args;
  if (std::ifstream in("/proc/self/cmdline", std::ios::binary); in) {
    std::string arg;
    while (std::getline(in, arg, '\0')) args.push_back(arg);
  }
  if (args.empty()) args.push_back("asura_wrath");
  auto game_log = [](std::string path) {
    std::filesystem::path p(path);
    return (p.parent_path() / (p.stem().string() + "-game" + p.extension().string())).string();
  };
  std::vector<std::string> game_args = {args[0]};
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--launcher" || a.rfind("--launcher=", 0) == 0) {
      // "--launcher false" (a separate value) as well.
      if (a == "--launcher" && i + 1 < args.size() && args[i + 1].rfind("-", 0) != 0) ++i;
      continue;
    }
    if (a.rfind("--log_file=", 0) == 0) {
      game_args.push_back("--log_file=" + game_log(a.substr(11)));
    } else if (a == "--log_file" && i + 1 < args.size()) {
      game_args.push_back(a);
      game_args.push_back(game_log(args[++i]));
    } else {
      game_args.push_back(a);
    }
  }
  game_args.push_back("--launcher=false");
  // The game applies Render resolution itself; only a value from the shell
  // is passed on.
  if (!RenderScaleFromEnvironment()) unsetenv("MANTRA_RENDER_SCALE");
  std::vector<char*> argv;
  for (auto& a : game_args) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t child = -1;
  const int rc = posix_spawn(&child, "/proc/self/exe", nullptr, nullptr, argv.data(), environ);
  if (rc != 0) {
    *error = std::string(T("Could not start the game: ")) + std::strerror(rc);
    return false;
  }
  REXLOG_INFO("launcher: started the game (pid {})", child);
  pid = child;
  stopping = false;
  return true;
}

void LauncherDialog::GameProcess::Stop() {
  if (pid <= 0 || stopping) return;
  kill(pid, SIGTERM);
  stopping = true;
  stop_time = std::chrono::steady_clock::now();
}

bool LauncherDialog::GameProcess::Poll(bool* failed) {
  *failed = false;
  if (pid <= 0) return false;
  int status = 0;
  const pid_t r = waitpid(pid, &status, WNOHANG);
  if (r == 0) {
    if (stopping && std::chrono::steady_clock::now() - stop_time > std::chrono::seconds(5)) {
      kill(pid, SIGKILL);
    }
    return true;
  }
  if (r == pid) {
    *failed = !stopping && !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    REXLOG_INFO("launcher: the game exited ({} {})",
                WIFEXITED(status) ? "status" : "signal",
                WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
  }
  pid = -1;
  stopping = false;
  return false;
}

void LauncherDialog::UseDroppedPath(const std::filesystem::path& path) {
  std::error_code ec;
  page_ = Page::kSettings;
  if (path.filename() == "default.xex") {
    UseDroppedPath(path.parent_path());
    return;
  }
  if (std::filesystem::is_directory(path, ec)) {
    if (std::filesystem::is_regular_file(path / "default.xex", ec)) {
      iso_path_.clear();
      SetGameData(path.string());
    } else {
      message_ = T("That folder has no default.xex.");
    }
    return;
  }
  if (IsDiscImage(path) || std::filesystem::is_regular_file(path, ec)) {
    if (iso_extract_.state == IsoExtract::kRunning) return;
    iso_path_ = path;
    // Default: a folder next to the image, or the user data folder if that
    // one is read-only.
    std::filesystem::path target = path.parent_path() / path.stem();
    if (access(path.parent_path().c_str(), W_OK) != 0) target = user_data_root_ / "game";
    std::snprintf(iso_target_buffer_, sizeof(iso_target_buffer_), "%s", target.string().c_str());
    iso_extract_.state = IsoExtract::kIdle;
    message_.clear();
  }
}

void LauncherDialog::OnFileDrop(rex::ui::FileDropEvent& e) {
  if (!playing_) UseDroppedPath(e.filename());
}

void LauncherDialog::OnKeyDown(rex::ui::KeyEvent& e) {
  if (playing_) return;
  last_input_ = std::chrono::steady_clock::now();
  if (pad_capture_ >= 0) {
    e.set_handled(true);
    if (e.virtual_key() == rex::ui::VirtualKey::kEscape) pad_capture_ = -1;
    return;
  }
  if (capture_flag_.empty()) return;
  e.set_handled(true);
  const rex::ui::VirtualKey vk = e.virtual_key();
  if (vk == rex::ui::VirtualKey::kEscape) {
    capture_flag_.clear();
    return;
  }
  // Modifiers alone can't be bound (the SDK reads them as part of a combo);
  // wait for the key they go with.
  if (IsModifierKey(vk)) return;
  const std::string name = rex::ui::VirtualKeyToString(vk);
  if (name.empty()) return;
  if (IsOverlayBind(capture_flag_)) {
    SetBind(capture_flag_, name);
    capture_flag_.clear();
    return;
  }
  std::string token;
  if (e.is_shift_pressed()) token += "Shift+";
  if (e.is_ctrl_pressed()) token += "Ctrl+";
  if (e.is_alt_pressed()) token += "Alt+";
  token += name;
  auto keys = SplitKeys(FlagValue(capture_flag_));
  keys.resize(std::max<size_t>(keys.size(), 2));
  keys[capture_slot_] = token;
  // The other slot keeps its key unless it is now the same.
  if (keys[1 - capture_slot_] == token) keys[1 - capture_slot_].clear();
  SetBind(capture_flag_, JoinKeys(keys));
  capture_flag_.clear();
}

void LauncherDialog::LimitFrameRate(ImGuiIO& io) {
  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  bool active = io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || io.MouseWheel != 0 ||
                ImGui::IsAnyMouseDown() || !io.InputQueueCharacters.empty() ||
                ImGui::IsAnyItemActive();
  // Things that move on their own: background jobs, the live controller view.
  active |= !capture_flag_.empty() || pad_capture_ >= 0 || page_ == Page::kControls ||
            shader_build_.state == ShaderBuild::kRunning ||
            iso_extract_.state == IsoExtract::kRunning || asset_job_.running;
  if (active) last_input_ = now;
  // 60 fps in use, 20 fps after 3 s without input (10 fps while the game
  // runs, to leave it the GPU).
  const bool idle = now - last_input_ > std::chrono::seconds(3);
  const auto period =
      std::chrono::microseconds(!idle ? 16667 : game_.Running() ? 100000 : 50000);
  if (last_frame_ != clock::time_point{} && now < last_frame_ + period) {
    std::this_thread::sleep_until(last_frame_ + period);
  }
  last_frame_ = std::max(clock::now(), last_frame_ + period);
}

void LauncherDialog::OnDraw(ImGuiIO& io) {
  if (playing_) return;
  LimitFrameRate(io);
  if (bool failed; game_.Running() && !game_.Poll(&failed)) {
    // The game may have changed settings (Esc in game), saves and
    // achievements.
    std::error_code ec;
    if (std::filesystem::last_write_time(settings_path_, ec) != settings_saved_time_ && !ec) {
      settings_.Load(settings_path_);
      std::snprintf(game_data_buffer_, sizeof(game_data_buffer_), "%s",
                    settings_.game_data.c_str());
      SetLauncherLanguage(settings_.language);
      settings_saved_time_ = std::filesystem::last_write_time(settings_path_, ec);
    }
    saves_scanned_ = false;
    achievements_loaded_ = false;
    if (failed) message_ = T("The game closed with an error (see the log).");
  }
  if (iso_extract_.state == IsoExtract::kDone) {
    if (iso_extract_.worker.joinable()) iso_extract_.worker.join();
    iso_extract_.state = IsoExtract::kIdle;
    iso_path_.clear();
    SetGameData(iso_extract_.out.string());
  }
  if (asset_job_.finished) {
    asset_job_.Join();
    achievements_loaded_ = false;
    bool failed;
    {
      std::lock_guard<std::mutex> lock(asset_job_.mutex);
      failed = !asset_job_.error.empty();
    }
    if (!failed) StartAssetJob();  // the language may have changed meanwhile
  }
  const ImVec2 display = io.DisplaySize;
  const float scale = std::clamp(std::min(display.y / 720.0f, display.x / 1280.0f), 0.6f, 3.0f);

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(display, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##asura_launcher", nullptr, flags)) {
    // The launcher has the window to itself: sidebar left, panel right.
    const float sidebar_w = std::floor(std::max(display.x * 0.21f, 200.0f * scale));
    DrawSidebar(ImVec2(0, 0), ImVec2(sidebar_w, display.y), scale);
    DrawPanel(ImVec2(sidebar_w, 0), ImVec2(display.x - sidebar_w, display.y), scale);
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}

// The game's Japanese title font only has the kanji of the game's own
// titles: Japanese headings use the text font, which has the rest.
ImFont* LauncherDialog::HeadingFont() const {
  return LauncherLanguage() == 2 ? font_ : title_font_;
}

bool LauncherDialog::SidebarButton(const char* label, bool selected, float width, float height) {
  if (button_.texture) {
    // The toast's band, scaled to the button's height and repeated along it
    // (it loops horizontally); warm when selected, brighter when hovered.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 to(at.x + width, at.y + height);
    const bool hovered = ImGui::IsMouseHoveringRect(at, to) && ImGui::IsWindowHovered();
    const float repeat = width / (button_.width * height / std::max(button_.height, 1.0f));
    const ImVec4 tint = selected  ? ImVec4(1.00f, 0.78f, 0.60f, 1.0f)
                        : hovered ? ImVec4(1.00f, 1.00f, 1.00f, 1.0f)
                                  : ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
    ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(button_.texture.get()), at,
                                         to, ImVec2(0, 0), ImVec2(repeat, 1), U32(tint));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button, selected ? kButtonSelected : kButton);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? kButtonSelected : kButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kButtonSelected);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.32f, 0.32f, 0.34f, 1.0f));
  }
  // Light text with a shadow-like weight: readable on the dark band.
  ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImVec4(1.0f, 0.93f, 0.84f, 1.0f)
                                                : ImVec4(0.92f, 0.90f, 0.86f, 1.0f));
  const bool clicked = ImGui::Button(label, ImVec2(width, height));
  ImGui::PopStyleColor(5);
  return clicked;
}

void LauncherDialog::DrawSidebar(const ImVec2& pos, const ImVec2& size, float scale) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), U32(kSidebar));
  // Subtle shadow on the panel side.
  draw->AddRectFilledMultiColor(ImVec2(pos.x + size.x - 6 * scale, pos.y),
                                ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 0));

  const float title_size = 30.0f * scale, button_size = 20.0f * scale;
  // Logo (art if present, else the title in the game's font), over the title
  // art when there is some.
  float y = pos.y + 28 * scale;
  if (title_.texture) {
    // The art's middle band, as wide as the sidebar, fading into it.
    const float w = size.x;
    const float h = std::floor(w * 0.72f);
    const float aspect = h / w * title_.width / std::max(title_.height, 1.0f);
    const float v0 = std::max(0.0f, 0.42f - aspect * 0.5f), v1 = std::min(1.0f, v0 + aspect);
    draw->AddImage(reinterpret_cast<ImTextureID>(title_.texture.get()), pos,
                   ImVec2(pos.x + w, pos.y + h), ImVec2(0, v0), ImVec2(1, v1));
    const ImU32 clear = U32(ImVec4(kSidebar.x, kSidebar.y, kSidebar.z, 0.0f)), solid = U32(kSidebar);
    draw->AddRectFilledMultiColor(ImVec2(pos.x, pos.y + h * 0.35f), ImVec2(pos.x + w, pos.y + h),
                                  clear, clear, solid, solid);
    y = pos.y + h * 0.62f;
  }
  const char* sub = "RECOMPILED";
  const float sub_size = 12.0f * scale;
  if (logo_.texture) {
    // The visible part of the logo, as wide as the sidebar allows.
    const float vis_w = (logo_.uv1.x - logo_.uv0.x) * logo_.width;
    const float vis_h = (logo_.uv1.y - logo_.uv0.y) * logo_.height;
    const float w = size.x - 28 * scale;
    const float h = std::min(w * vis_h / std::max(vis_w, 1.0f), 130 * scale);
    const float draw_w = h * vis_w / std::max(vis_h, 1.0f);
    const float x = pos.x + (size.x - draw_w) * 0.5f;
    draw->AddImage(reinterpret_cast<ImTextureID>(logo_.texture.get()), ImVec2(x, y),
                   ImVec2(x + draw_w, y + h), logo_.uv0, logo_.uv1);
    y += h + 4 * scale;
    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(sub_size, FLT_MAX, 0, sub);
    draw->AddText(ImGui::GetFont(), sub_size, ImVec2(pos.x + (size.x - ts.x) * 0.5f, y),
                  U32(kAccent), sub);
    y += ts.y + 36 * scale;
  } else {
    ImFont* font = title_font_ ? title_font_ : ImGui::GetFont();
    for (const char* text : {"ASURA'S", "WRATH"}) {
      const ImVec2 ts = font->CalcTextSizeA(title_size, FLT_MAX, 0, text);
      const ImVec2 at(pos.x + (size.x - ts.x) * 0.5f, y);
      draw->AddText(font, title_size, ImVec2(at.x + 2 * scale, at.y + 2 * scale),
                    IM_COL32(0, 0, 0, 190), text);  // shadow, readable over the art
      draw->AddText(font, title_size, at, IM_COL32(240, 236, 228, 255), text);
      y += ts.y;
    }
    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(sub_size, FLT_MAX, 0, sub);
    draw->AddText(ImGui::GetFont(), sub_size, ImVec2(pos.x + (size.x - ts.x) * 0.5f, y + 2 * scale),
                  U32(kAccent), sub);
    y += ts.y + 36 * scale;
  }

  ImGui::PushFont(font_, button_size);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.32f, 0.32f, 0.34f, 1.0f));
  const float button_h = 34 * scale, gap = 16 * scale;
  struct Item {
    const char* label;
    Page page;
  };
  for (const Item& item : {Item{"Controls", Page::kControls},
                           Item{"Achievements", Page::kAchievements},
                           Item{"Saves", Page::kSaves},
                           Item{"Graphics", Page::kGraphics},
                           Item{"System", Page::kSettings}}) {
    ImGui::SetCursorScreenPos(ImVec2(pos.x, y));
    ImGui::PushID(item.label);
    const bool clicked = SidebarButton(T(item.label), page_ == item.page, size.x, button_h);
    ImGui::PopID();
    if (clicked) {
      page_ = item.page;
      capture_flag_.clear();
      pad_capture_ = -1;
      if (page_ != Page::kControls) ClosePad();
      confirm_.clear();
      if (page_ == Page::kSaves) saves_scanned_ = false;
    }
    y += button_h + gap;
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  ImGui::PopFont();

  // Play, at the bottom.
  bool busy = shader_build_.state == ShaderBuild::kRunning ||
              iso_extract_.state == IsoExtract::kRunning;
  // A change that needs the shaders rebuilt (gone again if it's undone),
  // unless a message or the build itself is already showing.
  std::string notice = message_;
  if (notice.empty() && shader_build_.state != ShaderBuild::kRunning) {
    switch (ShaderCacheFitFor(settings_)) {
      case ShaderCacheFit::kMissing: notice = T("The shaders aren't built yet."); break;
      case ShaderCacheFit::kOutdated:
        notice = T("The new render resolution needs the shaders rebuilt.");
        break;
      case ShaderCacheFit::kReady: break;
    }
    if (!notice.empty()) notice += std::string(" ") + T("Graphics > Shaders");
  }
  if (!notice.empty()) {
    // In the launcher's font (ImGui's own has no Japanese).
    ImGui::PushFont(font_, 20.0f * scale);
    const float wrap = size.x - 20 * scale;
    const ImVec2 text_size = ImGui::CalcTextSize(notice.c_str(), nullptr, false, wrap);
    const ImVec2 at(pos.x + 10 * scale, pos.y + size.y - 100 * scale - text_size.y);
    ImGui::SetCursorScreenPos(at);
    ImGui::PushTextWrapPos(pos.x + size.x - 10 * scale);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.40f, 1.0f));
    ImGui::TextUnformatted(notice.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    if (message_.empty()) {
      // Click to open the Shaders tab.
      ImGui::SetCursorScreenPos(at);
      if (ImGui::InvisibleButton("##notice", ImVec2(wrap, text_size.y))) {
        page_ = Page::kGraphics;
        graphics_tab_request_ = kGraphicsShaders;
      }
      if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::PopFont();
  }
  ImGui::PushFont(font_, 34.0f * scale);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.12f));
  ImGui::PushStyleColor(ImGuiCol_Text, busy ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f)
                                            : ImVec4(0.96f, 0.95f, 0.92f, 1.0f));
  ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y - 90 * scale));
  // Over the running game: Resume, or Restart for changes that need one.
  const bool in_game = bool(in_game_needs_restart_);
  const bool restart = in_game && in_game_needs_restart_(settings_);
  // The game started from here: Stop while it runs.
  const bool stop = !in_game && game_.Running();
  if (stop) busy = false;  // the jobs wait for the game instead
  const char* play_label = stop       ? T("Stop")
                           : !in_game ? T("Play")
                           : restart  ? T("Restart")
                                      : T("Resume");
  const bool clicked =
      ImGui::Button((std::string(play_label) + "##play").c_str(), ImVec2(size.x, 64 * scale));
  const bool esc_resume = std::exchange(resume_requested_, false) && in_game;
  const bool play = (clicked || esc_resume) && !busy;
  const bool restart_chosen = clicked && restart;
  ImGui::PopStyleColor(4);
  ImGui::PopFont();
  if (ImGui::IsItemHovered() && stop) {
    ImGui::PushFont(font_, 22.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * scale, 10 * scale));
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(T("Closes the game: progress since the last save is lost."));
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextDisabled("%s", T("Settings changed here apply the next time the game starts. In "
                                "game, Esc changes them right away."));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
    ImGui::PopFont();
  }
  if (ImGui::IsItemHovered() && restart && !busy) {
    ImGui::PushFont(font_, 22.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * scale, 10 * scale));
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(T("Restarts the game: progress since the last save is lost."));
    ImGui::TextDisabled("%s", T("Esc goes back to the game; the change then applies next time."));
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
    ImGui::PopFont();
  }
  if (ImGui::IsItemHovered() && busy) {
    // In the launcher's font and size (tooltips otherwise use ImGui's), with
    // what the task is doing.
    std::string detail;
    float fraction = -1.0f;
    if (shader_build_.state == ShaderBuild::kRunning) {
      const int done = shader_build_.done, total = shader_build_.total;
      char text[128];
      if (total) {
        std::snprintf(text, sizeof(text), T("%d of %d shaders"), done, total);
        fraction = float(done) / float(total);
      } else {
        std::snprintf(text, sizeof(text), "%s", T("Reading the game files..."));
      }
      detail = std::string(T("Building the shader cache")) + ": " + text;
    } else {
      const uint64_t total = iso_extract_.total_bytes, done = iso_extract_.done_bytes;
      if (total) fraction = float(double(done) / double(total));
      detail = iso_extract_.Message();
    }
    ImGui::PushFont(font_, 22.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * scale, 10 * scale));
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(T("Wait for the current task to finish"));
    if (!detail.empty()) ImGui::TextDisabled("%s", detail.c_str());
    if (fraction >= 0.0f) {
      ImGui::ProgressBar(fraction, ImVec2(360 * scale, 8 * scale), "");
    }
    ImGui::EndTooltip();
    ImGui::PopStyleVar();
    ImGui::PopFont();
  }

  if (play && stop) {
    game_.Stop();
    return;
  }
  if (play) {
    settings_.game_data = game_data_buffer_;
    if (!GameDataValid()) {
      message_ = T("Set the game folder in System (it must contain default.xex).");
      page_ = Page::kSettings;
      return;
    }
    // Without the shader cache the game stutters on every new shader: build
    // it first (a second Play starts anyway).
    const ShaderCacheFit fit = ShaderCacheFitFor(settings_);
    if (!in_game && fit != ShaderCacheFit::kReady && !play_without_shaders_) {
      message_ = fit == ShaderCacheFit::kMissing
                     ? T("Build the shader cache first (or press Play again to start without it).")
                     : T("Rebuild the shaders for the new render resolution (or press Play again "
                         "to start anyway).");
      page_ = Page::kGraphics;
      graphics_tab_request_ = kGraphicsShaders;
      play_without_shaders_ = true;
      return;
    }
    capture_flag_.clear();
    pad_capture_ = -1;
    ClosePad();
    SaveSettings();
    if (!in_game) {
      message_.clear();
      if (std::string error; !game_.Start(&error)) message_ = error;
      play_without_shaders_ = false;
      return;
    }
    playing_ = true;
    // After this frame: the callback may destroy the launcher.
    imgui_drawer()->RemoveDialog(this);
    call_in_ui_thread_deferred_(
        [cb = on_resume_, s = settings_, restart_chosen] { cb(s, restart_chosen); });
  }
}

void LauncherDialog::DrawPanel(const ImVec2& pos, const ImVec2& size, float scale) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 end(pos.x + size.x, pos.y + size.y);
  float band = std::floor(size.y * 0.08f);
  // Where the page goes: between the bands, or inside the background's
  // inner panel.
  ImVec2 content_min(pos.x, pos.y + band), content_max(end.x, end.y - band);
  float title_x = pos.x + 36 * scale;
  if (panel_.texture) {
    // Nine-slice: the frame (outside the slice lines, in image pixels) is
    // scaled uniformly, so it keeps its look at any window size; only the
    // edges' middles and the center stretch.
    constexpr float kSlice[4] = {200, 140, 200, 140};          // left, top, right, bottom
    constexpr float kInner[4] = {222, 165, 1778, 952};         // the inner panel (2000x1125)
    const float iw = panel_.width, ih = panel_.height;
    const float k = std::min(size.x / iw, size.y / ih) * 0.8f;
    const float xs[4] = {pos.x, pos.x + kSlice[0] * k, end.x - kSlice[2] * k, end.x};
    const float ys[4] = {pos.y, pos.y + kSlice[1] * k, end.y - kSlice[3] * k, end.y};
    const float us[4] = {0, kSlice[0] / iw, 1 - kSlice[2] / iw, 1};
    const float vs[4] = {0, kSlice[1] / ih, 1 - kSlice[3] / ih, 1};
    const ImTextureID id = reinterpret_cast<ImTextureID>(panel_.texture.get());
    for (int j = 0; j < 3; ++j) {
      for (int i = 0; i < 3; ++i) {
        draw->AddImage(id, ImVec2(xs[i], ys[j]), ImVec2(xs[i + 1], ys[j + 1]), ImVec2(us[i], vs[j]),
                       ImVec2(us[i + 1], vs[j + 1]));
      }
    }
    // Image pixel -> screen, through the same slices.
    auto map = [](float v, float image_size, float slice0, float slice1, const float* screen) {
      if (v <= slice0) return screen[0] + (screen[1] - screen[0]) * v / slice0;
      if (v >= image_size - slice1)
        return screen[2] + (screen[3] - screen[2]) * (v - (image_size - slice1)) / slice1;
      return screen[1] + (screen[2] - screen[1]) * (v - slice0) / (image_size - slice0 - slice1);
    };
    content_min = ImVec2(map(kInner[0], iw, kSlice[0], kSlice[2], xs),
                         map(kInner[1], ih, kSlice[1], kSlice[3], ys));
    content_max = ImVec2(map(kInner[2], iw, kSlice[0], kSlice[2], xs),
                         map(kInner[3], ih, kSlice[1], kSlice[3], ys));
    band = content_min.y - pos.y;
    title_x = content_min.x;
  } else {
    draw->AddRectFilled(pos, end, U32(kPanelBand));
    draw->AddRectFilled(ImVec2(pos.x, pos.y + band), ImVec2(end.x, end.y - band), U32(kPanel));
  }

  // The page title, centered in the top band.
  const float pad = 36 * scale;
  {
    const char* title = "";
    switch (page_) {
      case Page::kControls: title = T("Controls"); break;
      case Page::kAchievements: title = T("Achievements"); break;
      case Page::kSaves: title = T("Saves"); break;
      case Page::kGraphics: title = T("Graphics"); break;
      case Page::kSettings: title = T("System"); break;
    }
    ImFont* font = HeadingFont() ? HeadingFont() : ImGui::GetFont();
    const float title_size = std::min(28.0f * scale, band * 0.6f);
    const ImVec2 ts = font->CalcTextSizeA(title_size, FLT_MAX, 0, title);
    draw->AddText(font, title_size, ImVec2(title_x, pos.y + std::floor((band - ts.y) * 0.5f)),
                  U32(kPanelText), title);
  }

  // The page, between the bands.
  const float inner = 22 * scale;
  const float side = panel_.texture ? inner : pad;
  ImGui::SetCursorScreenPos(ImVec2(content_min.x + side, content_min.y + inner));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, kPanelText);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, kPanelTextDim);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kField);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_CheckMark, kPanelText);
  ImGui::PushStyleColor(ImGuiCol_Button, kField);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, kField);
  ImGui::PushStyleColor(ImGuiCol_Header, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_Separator, kPanelBand);
  ImGui::PushStyleColor(ImGuiCol_TableBorderLight, kPanelBand);
  ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1, 1, 1, 0.07f));
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0.05f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, kPanelBand);
  ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4(0.30f, 0.26f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Tab, kPanelBand);
  ImGui::PushStyleColor(ImGuiCol_TabHovered, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_TabSelected, kField);
  ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kAccent);
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kPanelBand);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kAccent);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * scale, 5 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * scale, 9 * scale));
  // The game font draws smaller than ImGui's for the same size.
  ImGui::PushFont(font_, (font_ ? 18.0f : 15.0f) * scale);
  if (ImGui::BeginChild("##panel", ImVec2(content_max.x - content_min.x - 2 * side,
                                          content_max.y - content_min.y - 2 * inner))) {
    switch (page_) {
      case Page::kControls: DrawControls(scale); break;
      case Page::kAchievements: DrawAchievements(scale); break;
      case Page::kSaves: DrawSaves(scale); break;
      case Page::kGraphics: DrawGraphics(scale); break;
      case Page::kSettings: DrawSettings(scale); break;
    }
  }
  ImGui::EndChild();
  ImGui::PopFont();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(26);
}

namespace {
void SubHeading(ImFont* font, const char* text, float scale) {
  ImGui::Spacing();
  ImGui::PushFont(font, 19.0f * scale);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
}

// "(set by command line)" after a control whose flag was set elsewhere.
bool LockedNote(std::string_view flag) {
  if (!SetElsewhere(flag)) return false;
  ImGui::SameLine();
  ImGui::TextDisabled("%s", rex::cvar::GetFlagSource(flag) == rex::cvar::Source::kCommandLine
                                ? T("(set by command line)")
                                : T("(set by config)"));
  return true;
}
}  // namespace

namespace {

// Controller rows, in pad_remap.h order.
constexpr const char* kPadLabels[input::kPadControlCount] = {
    "A",          "B",           "X",          "Y",
    "Left shoulder", "Right shoulder", "Left trigger", "Right trigger",
    "Back",       "Start",       "Left stick press", "Right stick press",
    "D-pad up",   "D-pad down",  "D-pad left", "D-pad right",
};
// Short names on the live view and the rebind buttons.
constexpr const char* kPadShort[input::kPadControlCount] = {
    "A",  "B",  "X",  "Y",  "LB", "RB", "LT", "RT",
    "Back", "Start", "LS", "RS", "Up", "Down", "Left", "Right",
};

const char* PadShort(int control) {
  // Back/Start/LB... are printed on the pad; the d-pad directions are words.
  return control >= input::kPadUp ? T(kPadShort[control]) : kPadShort[control];
}

// Opens `folder` in the file manager.
bool OpenFolder(const std::filesystem::path& folder) {
  const std::string dir = folder.string();
  const char* argv[] = {"xdg-open", dir.c_str(), nullptr};
  pid_t pid = -1;
  if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0) {
    return false;
  }
  std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
  return true;
}

// ImGui's table header in light text on a dark band (readable on the panel).
void HeadersRow() {
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.92f, 0.86f, 1.0f));
  ImGui::TableHeadersRow();
  ImGui::PopStyleColor();
}

// A progress bar whose text stays readable over both the filled part (light
// text) and the empty part (dark text).
void ProgressBarText(float fraction, float height, const char* text) {
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  ImGui::Dummy(ImVec2(width, height));
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 end(pos.x + width, pos.y + height);
  const float split = pos.x + width * std::clamp(fraction, 0.0f, 1.0f);
  draw->AddRectFilled(pos, end, ImGui::GetColorU32(ImGuiCol_FrameBg));
  draw->AddRectFilled(pos, ImVec2(split, end.y), ImGui::GetColorU32(ImGuiCol_PlotHistogram));
  const ImVec2 ts = ImGui::CalcTextSize(text);
  const ImVec2 at(pos.x + (width - ts.x) * 0.5f, pos.y + (height - ts.y) * 0.5f);
  draw->PushClipRect(pos, ImVec2(split, end.y), true);
  draw->AddText(at, IM_COL32(250, 246, 238, 255), text);
  draw->PopClipRect();
  draw->PushClipRect(ImVec2(split, pos.y), end, true);
  draw->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), text);
  draw->PopClipRect();
}

// The page's "Reset to defaults" button, on its own line at the end.
bool ResetButton(const char* id) {
  ImGui::Spacing();
  ImGui::PushID(id);
  const bool clicked = ImGui::Button(T("Reset to defaults"));
  ImGui::PopID();
  return clicked;
}

}  // namespace

bool LauncherDialog::ReadPad(rex::input::X_INPUT_GAMEPAD* out) {
  using namespace rex::input;
  if (!pad_subsystem_) {
    // Released when the launcher closes, before the game's SDL driver starts
    // (it opens the pads from the add events SDL sends at init).
    pad_subsystem_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!pad_subsystem_) return false;
  }
  SDL_UpdateGamepads();
  if (pad_ && !SDL_GamepadConnected(pad_)) ClosePad();
  if (!pad_) {
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
      if (count > 0) pad_ = SDL_OpenGamepad(ids[0]);
      SDL_free(ids);
    }
    if (!pad_) return false;
    const char* name = SDL_GetGamepadName(pad_);
    pad_name_ = name ? name : "";
  }
  // As the SDK's SDL driver converts them (sdl_input_driver.cpp).
  static constexpr uint16_t kButtons[] = {
      X_INPUT_GAMEPAD_A,           X_INPUT_GAMEPAD_B,          X_INPUT_GAMEPAD_X,
      X_INPUT_GAMEPAD_Y,           X_INPUT_GAMEPAD_BACK,       0 /* guide */,
      X_INPUT_GAMEPAD_START,       X_INPUT_GAMEPAD_LEFT_THUMB, X_INPUT_GAMEPAD_RIGHT_THUMB,
      X_INPUT_GAMEPAD_LEFT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER, X_INPUT_GAMEPAD_DPAD_UP,
      X_INPUT_GAMEPAD_DPAD_DOWN,   X_INPUT_GAMEPAD_DPAD_LEFT,  X_INPUT_GAMEPAD_DPAD_RIGHT,
  };
  uint16_t buttons = 0;
  for (int i = 0; i < int(std::size(kButtons)); ++i) {
    if (SDL_GetGamepadButton(pad_, SDL_GamepadButton(i))) buttons |= kButtons[i];
  }
  *out = {};
  out->buttons = buttons;
  out->thumb_lx = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX);
  out->thumb_ly = int16_t(~SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTY));
  out->thumb_rx = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTX);
  out->thumb_ry = int16_t(~SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTY));
  out->left_trigger = uint8_t(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
  out->right_trigger = uint8_t(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
  return true;
}

void LauncherDialog::ClosePad() {
  if (pad_) SDL_CloseGamepad(pad_);
  pad_ = nullptr;
  pad_name_.clear();
}

void LauncherDialog::SetPadSource(int target, int source) {
  input::PadMap map = PadMapFrom(settings_);
  // Swap with the button that had it, so every button stays reachable.
  for (int other = 0; other < input::kPadControlCount; ++other) {
    if (other != target && map[other] == source) map[other] = map[target];
  }
  map[target] = uint8_t(source);
  settings_.pad.clear();
  for (int t = 0; t < input::kPadControlCount; ++t) {
    if (map[t] != t) settings_.pad[input::PadControlName(t)] = input::PadControlName(map[t]);
  }
  input::SetPadMap(map);
  SaveSettings();
}

void LauncherDialog::DrawControls(float scale) {
  if (ImGui::BeginTabBar("##controls_tabs")) {
    if (ImGui::BeginTabItem(T("Controller"))) {
      if (controls_tab_ != 0) capture_flag_.clear();
      controls_tab_ = 0;
      ImGui::Spacing();
      DrawController(scale);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(T("Keyboard"))) {
      if (controls_tab_ != 1) pad_capture_ = -1;
      controls_tab_ = 1;
      ImGui::Spacing();
      DrawKeyboard(scale);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
}

void LauncherDialog::DrawController(float scale) {
  using rex::input::X_INPUT_GAMEPAD;
  X_INPUT_GAMEPAD raw = {};
  const bool connected = ReadPad(&raw);
  const input::PadMap map = PadMapFrom(settings_);
  X_INPUT_GAMEPAD mapped = raw;
  input::RemapPad(mapped, map);

  if (connected) {
    ImGui::Text(T("Controller: %s"), pad_name_.empty() ? "?" : pad_name_.c_str());
  } else {
    ImGui::TextDisabled("%s", T("No controller found. Connect one to test it here."));
  }

  // Rebinding: the first control pressed after the click.
  if (pad_capture_ >= 0 && connected) {
    uint32_t down = 0;
    for (int c = 0; c < input::kPadControlCount; ++c) {
      if (input::PadDown(raw, c)) down |= 1u << c;
    }
    pad_capture_held_ &= down;  // released ones may be pressed again
    const uint32_t fresh = down & ~pad_capture_held_;
    if (fresh) {
      SetPadSource(pad_capture_, std::countr_zero(fresh));
      pad_capture_ = -1;
    }
  }

  // Live view: triggers, sticks (deadzone shaded) and what the game sees.
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float radius = 46 * scale, bar_w = 16 * scale, gap = 24 * scale;
  const float view_h = radius * 2 + ImGui::GetTextLineHeight() + 6 * scale;
  ImGui::Dummy(ImVec2(0, view_h));
  const ImU32 line = U32(kPanelText), accent = U32(kAccent), field = U32(kField);
  const ImU32 dim = IM_COL32(0, 0, 0, 40);
  float x = origin.x;
  auto trigger = [&](uint8_t value, const char* label) {
    const ImVec2 a(x, origin.y), b(x + bar_w, origin.y + radius * 2);
    draw->AddRectFilled(a, b, field);
    const float fill = (b.y - a.y) * value / 255.0f;
    draw->AddRectFilled(ImVec2(a.x, b.y - fill), b,
                        value > rex::input::X_INPUT_GAMEPAD_TRIGGER_THRESHOLD ? accent : dim);
    draw->AddRect(a, b, line);
    draw->AddText(ImVec2(a.x, b.y + 4 * scale), line, label);
    x += bar_w + gap;
  };
  auto stick = [&](int16_t sx, int16_t sy, double deadzone, const char* label) {
    const ImVec2 c(x + radius, origin.y + radius);
    draw->AddCircleFilled(c, radius, field, 48);
    if (deadzone > 0) draw->AddCircleFilled(c, float(radius * deadzone), dim, 32);
    draw->AddCircle(c, radius, line, 48);
    const float fx = sx / 32767.0f, fy = -sy / 32767.0f;
    const bool moving = std::sqrt(fx * fx + fy * fy) > deadzone;
    draw->AddCircleFilled(ImVec2(c.x + fx * radius, c.y + fy * radius), 6 * scale,
                          moving ? accent : line, 16);
    char text[48];
    std::snprintf(text, sizeof(text), "%s  %+.2f %+.2f", label, fx, -fy);
    draw->AddText(ImVec2(x, origin.y + radius * 2 + 4 * scale), line, text);
    x += radius * 2 + gap;
  };
  trigger(mapped.left_trigger, "LT");
  stick(raw.thumb_lx, raw.thumb_ly, settings_.deadzone_left, "L");
  stick(raw.thumb_rx, raw.thumb_ry, settings_.deadzone_right, "R");
  trigger(mapped.right_trigger, "RT");
  // Buttons the game sees (after the remap), two rows.
  {
    const float chip_w = 58 * scale, chip_h = 26 * scale, chip_gap = 6 * scale;
    for (int c = 0; c < input::kPadControlCount; ++c) {
      if (c == input::kPadLT || c == input::kPadRT) continue;
      const int slot = c < input::kPadLT ? c : c - 2;  // 14 chips, 7 per row
      const ImVec2 a(x + (slot % 7) * (chip_w + chip_gap),
                     origin.y + 10 * scale + (slot / 7) * (chip_h + chip_gap));
      const ImVec2 b(a.x + chip_w, a.y + chip_h);
      const bool down = input::PadDown(mapped, c);
      draw->AddRectFilled(a, b, down ? accent : field, 4 * scale);
      draw->AddRect(a, b, line, 4 * scale);
      const char* label = PadShort(c);
      const ImVec2 ts = ImGui::CalcTextSize(label);
      draw->AddText(ImVec2(a.x + (chip_w - ts.x) * 0.5f, a.y + (chip_h - ts.y) * 0.5f),
                    down ? IM_COL32(250, 246, 238, 255) : line, label);
    }
  }
  ImGui::Spacing();

  // Deadzones.
  const float label_w = 220 * scale;
  auto deadzone = [&](const char* label, const char* flag, double* value) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(260 * scale);
    float percent = float(*value * 100.0);
    ImGui::BeginDisabled(SetElsewhere(flag));
    ImGui::PushID(flag);
    if (ImGui::SliderFloat("##deadzone", &percent, 0.0f, 50.0f, "%.0f%%")) {
      *value = std::round(percent) / 100.0;
      SetIfUnset(flag, std::to_string(*value));
      SaveSettings();
    }
    ImGui::PopID();
    ImGui::EndDisabled();
    LockedNote(flag);
  };
  deadzone(T("Left stick deadzone"), "left_stick_deadzone_percentage", &settings_.deadzone_left);
  deadzone(T("Right stick deadzone"), "right_stick_deadzone_percentage", &settings_.deadzone_right);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(T("Vibration"));
  ImGui::SameLine(label_w);
  ImGui::SetNextItemWidth(260 * scale);
  if (ImGui::SliderInt("##vibration", &settings_.vibration, 0, 200,
                       settings_.vibration ? "%d%%" : T("Off"))) {
    input::SetVibrationStrength(settings_.vibration);
    SaveSettings();
  }

  ImGui::Spacing();
  ImGui::TextDisabled("%s", pad_capture_ >= 0
                                ? T("Press the controller button to use, Esc to cancel.")
                                : T("Click a button to change it, right-click to reset it."));
  const float table_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
  const ImGuiTableFlags table_flags =
      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
  if (ImGui::BeginTable("##pad", 3, table_flags, ImVec2(0, std::max(table_h, 120 * scale)))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(T("Button"), ImGuiTableColumnFlags_WidthFixed, 200 * scale);
    ImGui::TableSetupColumn(T("Controller button"));
    ImGui::TableSetupColumn("##live", ImGuiTableColumnFlags_WidthFixed, 30 * scale);
    HeadersRow();
    for (int t = 0; t < input::kPadControlCount; ++t) {
      ImGui::PushID(t);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(T(kPadLabels[t]));
      ImGui::TableNextColumn();
      const bool capturing = pad_capture_ == t;
      std::string text = capturing ? std::string(T("Press a button...")) : PadShort(map[t]);
      if (!capturing && map[t] != t) text += "  *";
      if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
      if (ImGui::Button(text.c_str(), ImVec2(-1, 0))) {
        pad_capture_ = t;
        // Ignore what is already held (e.g. the button that clicked).
        pad_capture_held_ = 0;
        for (int c = 0; c < input::kPadControlCount; ++c) {
          if (input::PadDown(raw, c)) pad_capture_held_ |= 1u << c;
        }
      }
      if (capturing) ImGui::PopStyleColor();
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        SetPadSource(t, t);
        pad_capture_ = -1;
      }
      ImGui::TableNextColumn();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float h = ImGui::GetFrameHeight();
      draw = ImGui::GetWindowDrawList();
      draw->AddCircleFilled(ImVec2(p.x + 10 * scale, p.y + h * 0.5f), 6 * scale,
                            input::PadDown(mapped, t) ? accent : dim, 16);
      ImGui::Dummy(ImVec2(20 * scale, h));
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (ResetButton("pad")) {
    pad_capture_ = -1;
    settings_.pad.clear();
    input::SetPadMap(input::IdentityPadMap());
    settings_.deadzone_left = settings_.deadzone_right = 0;
    settings_.vibration = 100;
    input::SetVibrationStrength(100);
    SetIfUnset("left_stick_deadzone_percentage", "0");
    SetIfUnset("right_stick_deadzone_percentage", "0");
    SaveSettings();
  }
}

void LauncherDialog::DrawKeyboard(float scale) {
  ImGui::BeginDisabled(SetElsewhere("mnk_mode"));
  if (ImGui::Checkbox(T("Play with the keyboard"), &settings_.keyboard)) {
    capture_flag_.clear();
    SaveSettings();
  }
  ImGui::EndDisabled();
  LockedNote("mnk_mode");

  {
    ImGui::BeginDisabled(!settings_.keyboard);
    bool mouse = FlagValue("mnk_mouse") == "true";
    ImGui::BeginDisabled(SetElsewhere("mnk_mouse"));
    if (ImGui::Checkbox(T("Mouse moves the camera"), &mouse)) {
      SetBind("mnk_mouse", mouse ? "true" : "false");
    }
    ImGui::EndDisabled();
    LockedNote("mnk_mouse");
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextDisabled("%s", capture_flag_.empty()
                                  ? T("Click a key to change it, right-click to clear it.")
                                  : T("Press the new key (combos like Shift+Up work), Esc to cancel."));
    const float table_h =
        ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##binds", 3, table_flags, ImVec2(0, std::max(table_h, 120 * scale)))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn(T("Button"), ImGuiTableColumnFlags_WidthFixed, 220 * scale);
      ImGui::TableSetupColumn(T("Key"));
      ImGui::TableSetupColumn(T("Alternate"));
      HeadersRow();
      auto bind_row = [&](const BindRow& row, bool enabled, int slots) {
        ImGui::PushID(row.flag);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T(row.label));
        const bool locked = SetElsewhere(row.flag) || !enabled;
        auto keys = SplitKeys(FlagValue(row.flag));
        keys.resize(std::max<size_t>(keys.size(), 2));
        for (int slot = 0; slot < slots; ++slot) {
          ImGui::TableNextColumn();
          ImGui::PushID(slot);
          const bool capturing = capture_flag_ == row.flag && capture_slot_ == slot;
          std::string text = capturing ? std::string(T("Press a key..."))
                             : keys[slot].empty() ? std::string("-")
                                                  : keys[slot];
          if (slot == 1 && keys.size() > 2) {
            for (size_t i = 2; i < keys.size(); ++i) {
              if (!keys[i].empty()) text += ", " + keys[i];
            }
          }
          ImGui::BeginDisabled(locked);
          if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
          if (ImGui::Button(text.c_str(), ImVec2(-1, 0))) {
            capture_flag_ = row.flag;
            capture_slot_ = slot;
          }
          if (capturing) ImGui::PopStyleColor();
          if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            keys[slot].clear();
            if (slot == 1) keys.resize(2);
            SetBind(row.flag, JoinKeys(keys));
            capture_flag_.clear();
          }
          ImGui::EndDisabled();
          if (SetElsewhere(row.flag) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", T("Set by the command line or config file"));
          }
          ImGui::PopID();
        }
        ImGui::PopID();
      };
      for (const BindRow& row : kBindRows) bind_row(row, settings_.keyboard, 2);
      // The overlays' keys work with a controller too.
      ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::PushFont(HeadingFont(), 18.0f * scale);
      ImGui::TextUnformatted(T("Overlays"));
      ImGui::PopFont();
      for (const BindRow& row : kOverlayRows) bind_row(row, true, 1);
      ImGui::EndTable();
    }
  }
  if (ResetButton("keys")) {
    for (const BindRow& row : kBindRows) SetBind(row.flag, FlagDefault(row.flag));
    for (const BindRow& row : kOverlayRows) SetBind(row.flag, FlagDefault(row.flag));
    SetBind("mnk_mouse", FlagDefault("mnk_mouse"));
    settings_.keyboard = false;
    capture_flag_.clear();
    SaveSettings();
  }
  ImGui::SameLine();
  ImGui::TextDisabled(T("In game: %s performance, %s settings, %s achievements, %s console."),
                      FlagValue("bind_debug_overlay").c_str(), FlagValue("bind_settings").c_str(),
                      FlagValue("bind_achievements").c_str(), FlagValue("bind_console").c_str());
}

void LauncherDialog::DrawAchievements(float scale) {
  if (!achievements_loaded_) LoadAchievements();
  if (achievements_.empty()) {
    if (asset_job_.running) {
      ImGui::TextUnformatted(T("Reading the achievement list from the game..."));
    } else if (!GameDataValid()) {
      ImGui::TextWrapped("%s", T("Set the game folder in System to see the achievement list."));
    } else {
      std::lock_guard<std::mutex> lock(asset_job_.mutex);
      ImGui::TextWrapped(T("Could not read the achievement list%s%s."),
                         asset_job_.error.empty() ? "" : ": ", asset_job_.error.c_str());
    }
    return;
  }
  int unlocked = 0;
  uint32_t total_gs = 0, earned_gs = 0;
  for (const auto& a : achievements_) {
    total_gs += a.gamerscore;
    if (a.unlocked_filetime) {
      ++unlocked;
      earned_gs += a.gamerscore;
    }
  }
  const int count = int(achievements_.size());
  const float fraction = float(unlocked) / float(count);

  // Summary, then the view and the filter on the right of the same line.
  const char* const kFilters[] = {T("All"), T("Unlocked"), T("Locked")};
  const float combo_w = 150 * scale;
  const char* const kViews[] = {T("List"), T("Grid")};
  const float view_w = ImGui::CalcTextSize(kViews[0]).x + ImGui::CalcTextSize(kViews[1]).x +
                       ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
  ImGui::AlignTextToFramePadding();
  ImGui::Text("%d / %d", unlocked, count);
  ImGui::SameLine();
  ImGui::TextDisabled("%uG / %uG", earned_gs, total_gs);
  ImGui::SameLine(ImGui::GetContentRegionMax().x - combo_w - view_w - ImGui::GetStyle().ItemSpacing.x);
  for (int v = 0; v < 2; ++v) {
    const bool selected = settings_.achievements_grid == (v == 1);
    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    if (selected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.96f, 0.93f, 1.0f));
    ImGui::PushID(v);
    if (ImGui::Button(kViews[v]) && !selected) {
      settings_.achievements_grid = v == 1;
      SaveSettings();
    }
    ImGui::PopID();
    if (selected) ImGui::PopStyleColor(2);
    ImGui::SameLine();
  }
  ImGui::SetNextItemWidth(combo_w);
  if (ImGui::BeginCombo("##filter", kFilters[achievement_filter_])) {
    for (int i = 0; i < 3; ++i) {
      if (ImGui::Selectable(kFilters[i], achievement_filter_ == i)) achievement_filter_ = i;
    }
    ImGui::EndCombo();
  }
  char percent[16];
  std::snprintf(percent, sizeof(percent), "%d%%", int(fraction * 100.0f + 0.5f));
  ProgressBarText(fraction, 22 * scale, percent);
  ImGui::Spacing();

  auto date_of = [](const Achievement& a) {
    std::string text;
    if (a.unlocked_filetime > 1) {
      // FILETIME: 100 ns ticks since 1601.
      const std::time_t t = std::time_t(a.unlocked_filetime / 10000000ull - 11644473600ull);
      char date[32];
      std::tm tm{};
      localtime_r(&t, &tm);
      std::strftime(date, sizeof(date), "%Y-%m-%d", &tm);
      text = date;
    }
    return text;
  };
  auto icon_tint = [](bool is_unlocked) {
    return is_unlocked ? ImVec4(1, 1, 1, 1) : ImVec4(0.45f, 0.42f, 0.40f, 0.55f);
  };
  auto shown = [&](const Achievement& a) {
    const bool is_unlocked = a.unlocked_filetime != 0;
    return !((achievement_filter_ == 1 && !is_unlocked) || (achievement_filter_ == 2 && is_unlocked));
  };
  const ImVec4 locked_card(1.0f, 0.97f, 0.88f, 0.08f);
  const float name_size = 20.0f * scale;

  if (ImGui::BeginChild("##achievements", ImVec2(0, 0))) {
    if (!settings_.achievements_grid) {
      // List: icon left; name and points, description; date on the right.
      const float icon = 72 * scale, pad = 10 * scale;
      ImDrawList* draw = ImGui::GetWindowDrawList();
      for (const auto& a : achievements_) {
        if (!shown(a)) continue;
        const bool is_unlocked = a.unlocked_filetime != 0;
        ImGui::PushID(int(a.id));
        const ImVec2 row_start = ImGui::GetCursorScreenPos();
        const float row_right = row_start.x + ImGui::GetContentRegionAvail().x - 6 * scale;
        draw->ChannelsSplit(2);
        draw->ChannelsSetCurrent(1);
        ImGui::SetCursorScreenPos(ImVec2(row_start.x + pad, row_start.y + pad));
        if (a.icon.texture) {
          ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(a.icon.texture.get()), ImVec2(icon, icon),
                             ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), icon_tint(is_unlocked));
        } else {
          ImGui::Dummy(ImVec2(icon, icon));
        }
        ImGui::SameLine(0, 14 * scale);
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, 2 * scale));
        ImGui::PushFont(font_, name_size);
        ImGui::PushStyleColor(ImGuiCol_Text, is_unlocked ? kPanelText : kPanelTextDim);
        ImGui::TextUnformatted(a.name.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%uG", a.gamerscore);
        const std::string date = date_of(a);
        if (!date.empty()) {
          ImGui::SameLine();
          const float date_w = ImGui::CalcTextSize(date.c_str()).x;
          ImGui::SetCursorScreenPos(ImVec2(row_right - date_w - pad, ImGui::GetCursorScreenPos().y));
          ImGui::TextDisabled("%s", date.c_str());
        }
        ImGui::PushStyleColor(ImGuiCol_Text, is_unlocked ? kPanelText : kPanelTextDim);
        ImGui::PushTextWrapPos(row_right - pad - ImGui::GetWindowPos().x);
        ImGui::TextWrapped("%s", (is_unlocked ? a.description : a.locked_description).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        const float row_bottom =
            std::max(row_start.y + icon + 2 * pad, ImGui::GetItemRectMax().y + pad);
        draw->ChannelsSetCurrent(0);
        draw->AddRectFilled(row_start, ImVec2(row_right, row_bottom),
                            U32(is_unlocked ? kUnlockedRow : locked_card), 4 * scale);
        draw->ChannelsMerge();
        ImGui::SetCursorScreenPos(ImVec2(row_start.x, row_bottom + 8 * scale));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
      }
    } else {
      // Grid: icon on top; name and points, description, date below.
      const float gap = 12 * scale, pad = 12 * scale, icon = 96 * scale;
      const float avail = ImGui::GetContentRegionAvail().x - 6 * scale;
      const int columns = std::max(1, int((avail + gap) / (230 * scale + gap)));
      const float card_w = std::floor((avail - gap * (columns - 1)) / columns);
      const float line = ImGui::GetTextLineHeightWithSpacing();
      const float card_h = pad + icon + 8 * scale + name_size * 1.3f + line * 3 + line + pad;
      int column = 0;
      for (const auto& a : achievements_) {
        if (!shown(a)) continue;
        const bool is_unlocked = a.unlocked_filetime != 0;
        if (column > 0) ImGui::SameLine(0, gap);
        ImGui::PushID(int(a.id));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, is_unlocked ? kUnlockedRow : locked_card);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4 * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        if (ImGui::BeginChild("##card", ImVec2(card_w, card_h), ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
          const float inner_w = card_w - 2 * pad;
          ImGui::SetCursorPosX(pad + (inner_w - icon) * 0.5f);
          if (a.icon.texture) {
            ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(a.icon.texture.get()),
                               ImVec2(icon, icon), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                               icon_tint(is_unlocked));
          } else {
            ImGui::Dummy(ImVec2(icon, icon));
          }
          ImGui::Dummy(ImVec2(0, 2 * scale));
          // Points on the right of the name's line.
          char points[16];
          std::snprintf(points, sizeof(points), "%uG", a.gamerscore);
          const float points_w = ImGui::CalcTextSize(points).x;
          const float name_y = ImGui::GetCursorPosY();
          ImGui::PushFont(font_, name_size);
          ImGui::PushStyleColor(ImGuiCol_Text, is_unlocked ? kPanelText : kPanelTextDim);
          ImGui::PushTextWrapPos(pad + inner_w - points_w - 8 * scale);
          ImGui::TextWrapped("%s", a.name.c_str());
          ImGui::PopTextWrapPos();
          ImGui::PopStyleColor();
          ImGui::PopFont();
          const float after_name = ImGui::GetCursorPosY();
          ImGui::SetCursorPos(ImVec2(pad + inner_w - points_w, name_y + 2 * scale));
          ImGui::TextDisabled("%s", points);
          ImGui::SetCursorPosY(after_name);
          ImGui::PushStyleColor(ImGuiCol_Text, is_unlocked ? kPanelText : kPanelTextDim);
          ImGui::PushTextWrapPos(pad + inner_w);
          ImGui::TextWrapped("%s", (is_unlocked ? a.description : a.locked_description).c_str());
          ImGui::PopTextWrapPos();
          ImGui::PopStyleColor();
          const std::string date = date_of(a);
          if (!date.empty()) {
            ImGui::SetCursorPosY(card_h - pad - ImGui::GetTextLineHeight());
            ImGui::TextDisabled("%s", date.c_str());
          }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (++column == columns) {
          column = 0;
          ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
        }
      }
    }
  }
  ImGui::EndChild();
}

void LauncherDialog::DrawGameFolder(float label_w, float scale) {
  auto label = [&](const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(label_w);
  };
  const bool extracting = iso_extract_.state == IsoExtract::kRunning;
  label(T("Game folder"));
  ImGui::SetNextItemWidth(-1);
  ImGui::BeginDisabled(extracting || SetElsewhere("game_data_root"));
  if (ImGui::InputTextWithHint("##game", T("extracted game folder, or an .iso"),
                               game_data_buffer_, sizeof(game_data_buffer_))) {
    const std::filesystem::path typed = game_data_buffer_;
    if (IsDiscImage(typed)) {
      UseDroppedPath(typed);
    } else {
      settings_.game_data = game_data_buffer_;
      message_.clear();
      SaveSettings();
    }
  }
  ImGui::EndDisabled();
  LockedNote("game_data_root");
  ImGui::SetCursorPosX(label_w);
  if (GameDataValid()) {
    ImGui::TextDisabled("%s", T("default.xex found"));
  } else if (iso_path_.empty()) {
    ImGui::TextDisabled("%s", T("Drop the extracted game folder or the disc .iso on this window."));
  }

  if (iso_path_.empty()) return;
  // A disc image: extract it to a folder (the game, the shader cache builder
  // and the UI art all read plain files).
  std::error_code ec;
  ImGui::Spacing();
  label(T("Disc image"));
  const auto size = std::filesystem::file_size(iso_path_, ec);
  ImGui::Text("%s (%s)", iso_path_.filename().string().c_str(), ec ? "?" : FormatBytes(size).c_str());
  label(T("Extract to"));
  ImGui::SetNextItemWidth(-1);
  ImGui::BeginDisabled(extracting);
  ImGui::InputText("##iso_target", iso_target_buffer_, sizeof(iso_target_buffer_));
  ImGui::EndDisabled();
  ImGui::SetCursorPosX(label_w);
  const int state = iso_extract_.state;
  if (state == IsoExtract::kRunning) {
    const uint64_t total = iso_extract_.total_bytes, done = iso_extract_.done_bytes;
    const std::string overlay = FormatBytes(done) + " / " + FormatBytes(total);
    ImGui::ProgressBar(total ? float(double(done) / double(total)) : 0.0f,
                       ImVec2(-90 * scale, 0), overlay.c_str());
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(-1, 0))) iso_extract_.cancel = true;
    ImGui::SetCursorPosX(label_w);
    ImGui::TextDisabled("%s", iso_extract_.Message().c_str());
  } else {
    ImGui::BeginDisabled(game_.Running());
    const bool extract =
        ImGui::Button(state == IsoExtract::kFailed ? T("Try again") : T("Extract"));
    ImGui::EndDisabled();
    if (extract) {
      iso_extract_.Start(iso_path_, std::filesystem::path(iso_target_buffer_));
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Dismiss"))) iso_path_.clear();
    if (state == IsoExtract::kFailed) {
      ImGui::SetCursorPosX(label_w);
      ImGui::TextDisabled("%s%s", T("Failed: "), iso_extract_.Message().c_str());
    }
  }
}

void LauncherDialog::DrawSettings(float scale) {
  bool changed = false;
  const float label_w = 200 * scale;
  auto label = [&](const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(label_w);
  };

  DrawGameFolder(label_w, scale);

  SubHeading(HeadingFont(), T("Game"), scale);
  label(T("Language"));
  ImGui::SetNextItemWidth(240 * scale);
  struct Language {
    uint32_t id;
    const char* name;  // in that language
  };
  // The languages on the disc (BCGame/CookedXbox360/*_LOC_<lang>.xxx).
  // Alphabetical by their own names.
  static constexpr Language kLanguages[] = {
      {3, "Deutsch"},  {1, "English"},  {5, "Español"},
      {4, "Français"}, {6, "Italiano"}, {2, "日本語"}};
  const char* language_name = "English";
  for (const auto& l : kLanguages) {
    if (l.id == settings_.language) language_name = l.name;
  }
  auto set_language = [&](uint32_t id) {
    settings_.language = id;
    SetIfUnset("user_language", std::to_string(id));
    SetLauncherLanguage(id);
    changed = true;
  };
  ImGui::BeginDisabled(SetElsewhere("user_language"));
  if (ImGui::BeginCombo("##language", language_name)) {
    for (const auto& l : kLanguages) {
      if (ImGui::Selectable(l.name, l.id == settings_.language)) set_language(l.id);
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  LockedNote("user_language");
  ImGui::SetCursorPosX(label_w);
  ImGui::TextDisabled("%s", T("The game's menus and subtitles, the achievements and this launcher."));

  SubHeading(HeadingFont(), T("Achievement pop-ups"), scale);
  label(T("Show achievements"));
  changed |= ImGui::Checkbox("##toasts", &settings_.toasts);
  ImGui::BeginDisabled(!settings_.toasts);
  label(T("Corner"));
  {
    const char* corners[] = {T("Top left"), T("Top right"), T("Bottom left"), T("Bottom right")};
    ImGui::SetNextItemWidth(260 * scale);
    changed |= ImGui::Combo("##corner", &settings_.toast_corner, corners, 4);
  }
  label(T("Duration"));
  {
    float seconds = float(settings_.toast_seconds);
    ImGui::SetNextItemWidth(260 * scale);
    if (ImGui::SliderFloat("##seconds", &seconds, 1.0f, 15.0f, T("%.1f s"))) {
      settings_.toast_seconds = std::round(seconds * 2.0f) / 2.0f;
      changed = true;
    }
  }
  ImGui::EndDisabled();

  // Only while the game's fonts aren't in use yet: there is nothing to do
  // once they are.
  if (!fonts_at_start_) {
  SubHeading(HeadingFont(), T("Launcher"), scale);
  label(T("Game fonts"));
  if (HaveGameFonts()) {
    ImGui::TextUnformatted(T("Extracted from the game"));
    ImGui::SameLine();
    ImGui::BeginDisabled(game_.Running());
    const bool reopen = on_relaunch_ && ImGui::Button(T("Reopen the launcher to use them"));
    ImGui::EndDisabled();
    if (reopen) {
      SaveSettings();
      on_relaunch_();
    }
  } else if (asset_job_.running) {
    ImGui::TextUnformatted(T("Reading them from the game..."));
  } else {
    ImGui::TextDisabled("%s", T("Read from the game folder once it is set"));
  }
  }

  // The game folder stays: it is where the game is, not a preference.
  if (ResetButton("settings")) {
    const LauncherSettings defaults;
    settings_.toasts = defaults.toasts;
    settings_.toast_corner = defaults.toast_corner;
    settings_.toast_seconds = defaults.toast_seconds;
    changed = true;
    if (!SetElsewhere("user_language")) set_language(1);
  }

  if (changed) {
    SetToastOptions({settings_.toasts, settings_.toast_corner, float(settings_.toast_seconds)});
    SaveSettings();
    // The achievement names follow the language.
    if (CachedAchievementLanguage() != settings_.language) StartAssetJob();
  }
}

void LauncherDialog::DrawGraphics(float scale) {
  bool changed = false;
  const float label_w = 200 * scale;
  auto label = [&](const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(label_w);
  };

  auto hint = [&](const char* text) {
    ImGui::SetCursorPosX(label_w);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
  };
  auto restart_note = [&](const char* text) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
  };
  if (!ImGui::BeginTabBar("##graphics_tabs")) return;
  auto tab = [&](const char* name, int index) {
    const ImGuiTabItemFlags flags =
        graphics_tab_request_ == index ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    if (!ImGui::BeginTabItem(name, nullptr, flags)) return false;
    if (graphics_tab_request_ == index) graphics_tab_request_ = -1;
    ImGui::Spacing();
    return true;
  };

  if (tab(T("Display"), kGraphicsDisplay)) {
  label(T("Graphics card"));
  ImGui::SetNextItemWidth(360 * scale);
  const std::string automatic =
      automatic_gpu_ >= 0 ? std::string(T("Automatic")) + " (" + gpus_[automatic_gpu_].name + ")"
                          : std::string(T("Automatic"));
  const std::string current = settings_.gpu >= 0 && size_t(settings_.gpu) < gpus_.size()
                                  ? gpus_[settings_.gpu].name
                                  : automatic;
  ImGui::BeginDisabled(SetElsewhere("vulkan_device"));
  if (ImGui::BeginCombo("##gpu", current.c_str())) {
    if (ImGui::Selectable((automatic + "##auto").c_str(), settings_.gpu < 0)) {
      settings_.gpu = -1;
      changed = true;
    }
    for (size_t i = 0; i < gpus_.size(); ++i) {
      ImGui::PushID(int(i));
      if (ImGui::Selectable(gpus_[i].name.c_str(), settings_.gpu == int(i))) {
        settings_.gpu = int(i);
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  LockedNote("vulkan_device");

  label(T("VSync"));
  ImGui::BeginDisabled(SetElsewhere("vulkan_allow_present_mode_immediate"));
  changed |= ImGui::Checkbox("##vsync", &settings_.vsync);
  ImGui::EndDisabled();
  LockedNote("vulkan_allow_present_mode_immediate");

  label(T("Display mode"));
  {
    const char* modes[] = {T("Windowed"), T("Borderless fullscreen"), T("Fullscreen (exclusive)")};
    int mode = !settings_.fullscreen ? 0 : settings_.exclusive ? 2 : 1;
    ImGui::SetNextItemWidth(360 * scale);
    ImGui::BeginDisabled(SetElsewhere("fullscreen"));
    if (ImGui::Combo("##display_mode", &mode, modes, 3)) {
      settings_.fullscreen = mode != 0;
      settings_.exclusive = mode == 2;
      changed = true;
    }
    ImGui::EndDisabled();
    LockedNote("fullscreen");
  }

  label(T("Monitor"));
  {
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    std::string current = T("Default");
    if (settings_.monitor > 0 && settings_.monitor <= count) {
      if (const char* name = SDL_GetDisplayName(displays[settings_.monitor - 1])) current = name;
    }
    ImGui::SetNextItemWidth(360 * scale);
    ImGui::BeginDisabled(SetElsewhere("monitor"));
    if (ImGui::BeginCombo("##monitor", current.c_str())) {
      if (ImGui::Selectable(T("Default"), settings_.monitor == 0)) {
        settings_.monitor = 0;
        changed = true;
      }
      for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetDisplayName(displays[i]);
        const std::string text = std::to_string(i + 1) + ": " + (name ? name : "?");
        if (ImGui::Selectable(text.c_str(), settings_.monitor == i + 1)) {
          settings_.monitor = i + 1;
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    SDL_free(displays);
  }

  // The monitor's own modes, largest first (windowed: any that fits).
  label(T("Resolution"));
  {
    const std::vector<ScreenSize> sizes = MonitorSizes(settings_.monitor);
    const int w = settings_.window_width > 0 ? settings_.window_width : 1280;
    const int h = settings_.window_height > 0 ? settings_.window_height : 720;
    const bool borderless = settings_.fullscreen && !settings_.exclusive;
    ImGui::SetNextItemWidth(260 * scale);
    ImGui::BeginDisabled(borderless || SetElsewhere("window_width"));
    if (ImGui::BeginCombo("##window", SizeLabel(w, h).c_str())) {
      for (const auto& size : sizes) {
        if (ImGui::Selectable(SizeLabel(size.width, size.height).c_str(),
                              size.width == w && size.height == h)) {
          // 1280x720 is the default (0 x 0: the guest video mode).
          const bool is_default = size.width == 1280 && size.height == 720;
          settings_.window_width = is_default ? 0 : size.width;
          settings_.window_height = is_default ? 0 : size.height;
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (!LockedNote("window_width") && borderless) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", T("(uses the screen's size)"));
    }
  }

  // Off: the game's own 1280x720, scaled to the window by the presenter (the
  // rendering before render scaling). On: mantra renders at render_height.
  label(T("Higher resolution"));
  if (ImGui::Checkbox(T("Render above the game's 1280 x 720 (experimental)"),
                      &settings_.native_render)) {
    changed = true;
  }
  hint(T("Some effects can show green areas."));

  label(T("Render resolution"));
  {
    struct RenderSize {
      const char* label;
      int height;
    };
    static const RenderSize kRenderSizes[] = {
        {"3840 x 2160 (16:9)", 2160}, {"2560 x 1440 (16:9)", 1440}, {"1920 x 1080 (16:9)", 1080},
        {"1600 x 900 (16:9)", 900},   {"1280 x 720 (16:9)", 720},   {"960 x 540 (16:9)", 540},
    };
    const int height = RenderHeight(settings_);
    std::string current = SizeLabel(height * 16 / 9, height);
    for (const auto& r : kRenderSizes) {
      if (r.height == height) current = r.label;
    }
    if (height == 720) current += std::string(" (") + T("the game's") + ")";
    ImGui::SetNextItemWidth(260 * scale);
    ImGui::BeginDisabled(!settings_.native_render ||
                         RenderScaleFromEnvironment());
    if (ImGui::BeginCombo("##render", current.c_str())) {
      for (const auto& r : kRenderSizes) {
        std::string text = r.label;
        if (r.height == 720) text += std::string(" (") + T("the game's") + ")";
        if (ImGui::Selectable(text.c_str(), r.height == height)) {
          settings_.render_height = r.height;
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    hint(T("Higher is sharper, cutscenes included, and needs a faster graphics card with more "
           "memory."));
  }

  label(T("Wide screens"));
  ImGui::SetNextItemWidth(360 * scale);
  const char* aspect_labels[] = {T("Fit: 16:9 with black bars"), T("Fill: crop the top and bottom"),
                                 T("Stretch: fill the screen, distorted")};
  int aspect = int(settings_.aspect);
  ImGui::BeginDisabled(SetElsewhere("present_letterbox"));
  if (ImGui::Combo("##aspect", &aspect, aspect_labels, IM_ARRAYSIZE(aspect_labels))) {
    settings_.aspect = AspectMode(aspect);
    changed = true;
  }
  ImGui::EndDisabled();
  LockedNote("present_letterbox");
  hint(T("On screens that aren't 16:9 (ultrawide, 16:10, 4:3), how the game's 16:9 picture "
         "fills the rest."));
  restart_note(T("Graphics card, VSync, monitor, exclusive fullscreen, render resolution and Fill "
                 "changes restart the game when you press Play."));
  if (ResetButton("display")) {
    const LauncherSettings defaults;
    settings_.gpu = defaults.gpu;
    settings_.vsync = defaults.vsync;
    settings_.fullscreen = defaults.fullscreen;
    settings_.exclusive = defaults.exclusive;
    settings_.monitor = defaults.monitor;
    settings_.window_width = defaults.window_width;
    settings_.window_height = defaults.window_height;
    settings_.aspect = defaults.aspect;
    settings_.render_height = defaults.render_height;
    settings_.native_render = defaults.native_render;
    changed = true;
  }
  ImGui::EndTabItem();
  }

  if (tab(T("Shaders"), kGraphicsShaders)) {
  label(T("Shader cache"));
  const std::filesystem::path cache = ShaderCachePath();
  std::error_code ec;
  const bool have_cache = std::filesystem::is_regular_file(cache, ec);
  const int state = shader_build_.state;
  if (state == ShaderBuild::kRunning) {
    const int done = shader_build_.done, total = shader_build_.total;
    char text[128];
    if (total) {
      std::snprintf(text, sizeof(text), T("%d of %d shaders"), done, total);
    } else {
      std::snprintf(text, sizeof(text), "%s", T("Reading the game files..."));
    }
    ProgressBarText(total ? float(done) / float(total) : 0.0f, ImGui::GetFrameHeight(), text);
  } else if (have_cache) {
    ImGui::Text(T("Ready (%.0f MB)"), double(std::filesystem::file_size(cache, ec)) / 1048576.0);
    // Shaders the game patched while playing (checked once a second).
    const double now = ImGui::GetTime();
    if (now - shaders_checked_at_ > 1.0) {
      shaders_checked_at_ = now;
      shaders_not_in_cache_ = ShadersNotInCache();
    }
    if (shaders_not_in_cache_) {
      ImGui::SetCursorPosX(label_w);
      ImGui::TextDisabled(T("%d shaders seen while playing are missing: rebuild to add them."),
                          int(shaders_not_in_cache_));
    }
  } else {
    ImGui::TextUnformatted(T("Missing: the game will stutter heavily without it"));
  }
  ImGui::SetCursorPosX(label_w);
  const std::filesystem::path tool = rex::filesystem::GetExecutableFolder() / "mantra_shadergen";
  const bool have_tool = std::filesystem::is_regular_file(tool, ec);
  // Not while the game runs: it reads the cache at start and adds to it.
  ImGui::BeginDisabled(state == ShaderBuild::kRunning || !have_tool || !GameDataValid() ||
                       iso_extract_.state == IsoExtract::kRunning || game_.Running());
  if (ImGui::Button(have_cache ? T("Rebuild") : T("Build (about 10 minutes)"))) {
    shader_build_.Start(tool, settings_.game_data, cache, ShaderDumpDirs());
    message_.clear();
    play_without_shaders_ = false;
  }
  ImGui::EndDisabled();
  if (state == ShaderBuild::kFailed) {
    ImGui::SetCursorPosX(label_w);
    const std::string line = shader_build_.Line();
    ImGui::TextDisabled("%s%s", T("Failed: "), line.c_str());
  }
  label(T("Location"));
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(cache.parent_path().string().c_str());
  ImGui::PopTextWrapPos();
  ImGui::SetCursorPosX(label_w);
  if (ImGui::Button(T("Open the folder"))) {
    std::filesystem::create_directories(cache.parent_path(), ec);
    OpenFolder(cache.parent_path());
  }
  if (!have_tool) {
    ImGui::SetCursorPosX(label_w);
    ImGui::TextDisabled("%s", T("mantra_shadergen not found next to the game"));
  }
  ImGui::EndTabItem();
  }

  if (tab(T("Enhancements"), kGraphicsEnhancements)) {
  label(T("Frame rate"));
  changed |= ImGui::Checkbox(T("Show it in the top-left corner"), &settings_.show_fps);

  label(T("Frame rate limit"));
  {
    // 30 to 240 in steps of 5; the far right is Unlimited (0).
    constexpr int kMin = 30, kMax = 240, kUnlimited = kMax + 5;
    int value = settings_.fps_cap <= 0 ? kUnlimited : std::clamp(settings_.fps_cap, kMin, kMax);
    std::string format;
    if (value == kUnlimited) {
      format = T("Unlimited");
    } else {
      format = "%d fps";
      if (value == 30) format += std::string(" (") + T("the game's") + ")";
    }
    ImGui::SetNextItemWidth(360 * scale);
    if (ImGui::SliderInt("##fps_cap", &value, kMin, kUnlimited, format.c_str(),
                         ImGuiSliderFlags_AlwaysClamp)) {
      value = (value + 2) / 5 * 5;
      settings_.fps_cap = value >= kUnlimited ? 0 : value;
      changed = true;
    }
    // Common rates one click away.
    for (int cap : {30, 60, 120, 144, 0}) {
      ImGui::SameLine();
      const std::string text = cap ? std::to_string(cap) : std::string(T("Unlimited"));
      if (ImGui::SmallButton((text + "##fps" + std::to_string(cap)).c_str())) {
        settings_.fps_cap = cap;
        changed = true;
      }
    }
    hint(T("Above 30 is experimental: parts of the game may run too fast or behave oddly."));
  }

  // Held buttons repeat (controller, after the remap).
  label(T("Turbo"));
  {
    struct TurboButton {
      const char* name;
      input::PadControl control;
    };
    static const TurboButton kTurbo[] = {{"A", input::kPadA},   {"B", input::kPadB},
                                         {"X", input::kPadX},   {"Y", input::kPadY},
                                         {"LB", input::kPadLB}, {"RB", input::kPadRB}};
    bool first = true;
    for (const auto& b : kTurbo) {
      if (!first) ImGui::SameLine(0, 18 * scale);
      first = false;
      bool on = settings_.turbo & (1u << b.control);
      if (ImGui::Checkbox(b.name, &on)) {
        settings_.turbo = on ? settings_.turbo | (1u << b.control) : settings_.turbo & ~(1u << b.control);
        input::SetTurboControls(settings_.turbo);
        changed = true;
      }
    }
  }
  hint(T("Hold a ticked button instead of mashing it (controller only)."));

  label(T("Mouse cursor"));
  changed |= ImGui::Checkbox(T("Hide it in game while the mouse isn't moving"),
                             &settings_.hide_cursor);
  label(T("Sound"));
  ImGui::BeginDisabled(SetElsewhere("audio_mute"));
  changed |= ImGui::Checkbox(T("Mute while the game window isn't focused"),
                             &settings_.mute_background);
  ImGui::EndDisabled();
  LockedNote("audio_mute");
  if (ResetButton("enhancements")) {
    const LauncherSettings defaults;
    settings_.show_fps = defaults.show_fps;
    settings_.fps_cap = defaults.fps_cap;
    settings_.turbo = defaults.turbo;
    settings_.hide_cursor = defaults.hide_cursor;
    settings_.mute_background = defaults.mute_background;
    input::SetTurboControls(settings_.turbo);
    changed = true;
  }
  ImGui::EndTabItem();
  }
  ImGui::EndTabBar();

  if (changed) SaveSettings();
}

// --- Game files ---------------------------------------------------------------------

void LauncherDialog::AssetJob::Start(std::filesystem::path game, uint32_t language, bool fonts,
                                     bool achievements) {
  Join();
  {
    std::lock_guard<std::mutex> lock(mutex);
    error.clear();
  }
  running = true;
  worker = std::thread([this, game = std::move(game), language, fonts, achievements] {
    std::string fail;
    if (fonts && !ExtractUiFonts(game, UiCacheDir(), &fail)) {
      REXLOG_WARN("launcher: game fonts: {}", fail);
    }
    if (achievements && !ExtractAchievementList(game, AchievementCacheDir(), language, &fail)) {
      REXLOG_WARN("launcher: achievement list: {}", fail);
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      error = fail;
    }
    running = false;
    finished = true;
  });
}

void LauncherDialog::AssetJob::Join() {
  if (worker.joinable()) worker.join();
  finished = false;
}

void LauncherDialog::StartAssetJob() {
  if (!GameDataValid() || asset_job_.running) return;
  const bool fonts = !HaveGameFonts();
  const bool achievements = CachedAchievementLanguage() != settings_.language;
  if (fonts || achievements) {
    asset_job_.Start(settings_.game_data, settings_.language, fonts, achievements);
  }
}

// --- Saves ----------------------------------------------------------------------------

namespace {

uint64_t FolderBytes(const std::filesystem::path& dir, std::filesystem::file_time_type* newest) {
  uint64_t bytes = 0;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    bytes += it->file_size(ec);
    if (newest) *newest = std::max(*newest, it->last_write_time(ec));
  }
  return bytes;
}

std::string FormatTime(std::filesystem::file_time_type t) {
  if (t == std::filesystem::file_time_type::min()) return "-";
  const auto sys = std::chrono::file_clock::to_sys(t);
  const std::time_t tt = std::chrono::system_clock::to_time_t(
      std::chrono::time_point_cast<std::chrono::system_clock::duration>(sys));
  std::tm tm{};
  localtime_r(&tt, &tm);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &tm);
  return text;
}

// The display name in an XCONTENT_DATA header (UTF-16BE at offset 8).
std::string HeaderDisplayName(const std::filesystem::path& header) {
  std::ifstream in(header, std::ios::binary);
  char raw[8 + 256] = {};
  if (!in.read(raw, sizeof(raw))) return {};
  std::string name;
  for (int i = 0; i < 128; ++i) {
    const uint32_t c = uint8_t(raw[8 + i * 2]) << 8 | uint8_t(raw[9 + i * 2]);
    if (!c) break;
    if (c < 0x80) {
      name += char(c);
    } else if (c < 0x800) {
      name += char(0xC0 | (c >> 6));
      name += char(0x80 | (c & 0x3F));
    } else {
      name += char(0xE0 | (c >> 12));
      name += char(0x80 | ((c >> 6) & 0x3F));
      name += char(0x80 | (c & 0x3F));
    }
  }
  return name;
}

bool IsProfileFolder(const std::filesystem::directory_entry& entry) {
  const std::string name = entry.path().filename().string();
  return entry.is_directory() && name.size() == 16 &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isxdigit(c); });
}

constexpr const char* kTitleFolder = "43430817";
constexpr const char* kSaveType = "00000001";  // XContentType saved game

}  // namespace

void LauncherDialog::ScanSaves() {
  saves_scanned_ = true;
  profiles_.clear();
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(user_data_root_, ec)) {
    if (!IsProfileFolder(entry)) continue;
    ProfileInfo profile;
    profile.xuid = entry.path().filename().string();
    const auto saves_dir = entry.path() / kTitleFolder / kSaveType;
    for (const auto& save : std::filesystem::directory_iterator(saves_dir, ec)) {
      if (!save.is_directory()) continue;
      SaveInfo info;
      info.name = save.path().filename().string();
      info.path = save.path();
      info.header = entry.path() / kTitleFolder / "Headers" / kSaveType / (info.name + ".header");
      info.display_name = HeaderDisplayName(info.header);
      auto newest = std::filesystem::file_time_type::min();
      info.bytes = FolderBytes(save.path(), &newest);
      info.modified = FormatTime(newest);
      for (const auto& file : std::filesystem::directory_iterator(save.path(), ec)) {
        if (file.is_regular_file(ec)) {
          info.files.emplace_back(file.path().filename().string(), file.file_size(ec));
        }
      }
      std::sort(info.files.begin(), info.files.end());
      for (const auto& [file, size] : info.files) {
        if (ReadSaveSummary(save.path() / file, &info.progress)) {
          info.has_progress = true;
          break;
        }
      }
      profile.saves.push_back(std::move(info));
    }
    ec.clear();
    if (!profile.saves.empty()) profiles_.push_back(std::move(profile));
  }
}

void LauncherDialog::DrawSaves(float scale) {
  if (!saves_scanned_) ScanSaves();
  if (!achievements_loaded_) LoadAchievements();
  const float label_w = 200 * scale;
  auto label = [&](const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(label_w);
  };
  auto open_folder = [&](const std::filesystem::path& folder) {
    if (!OpenFolder(folder)) save_message_ = T("Could not run xdg-open.");
  };

  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("%s", T("The game keeps one save with all your progress and loads it by "
                              "itself."));
  ImGui::PopTextWrapPos();
  if (!save_message_.empty()) ImGui::TextUnformatted(save_message_.c_str());

  if (profiles_.empty()) {
    SubHeading(HeadingFont(), T("Saved games"), scale);
    ImGui::TextWrapped("%s", T("No saves yet. The game makes one when you start a new game."));
    label(T("Folder"));
    ImGui::TextUnformatted(user_data_root_.string().c_str());
    ImGui::SetCursorPosX(label_w);
    if (ImGui::Button(T("Open the folder"))) open_folder(user_data_root_);
    return;
  }

  // One card per save (normally a single one: one profile, one save).
  for (const auto& profile : profiles_) {
    for (const auto& save : profile.saves) {
      ImGui::PushID(save.path.string().c_str());
      SubHeading(HeadingFont(), (save.display_name.empty() ? save.name : save.display_name).c_str(),
                 scale);
      label(T("Last saved"));
      ImGui::TextUnformatted(save.modified.c_str());
      label(T("Size"));
      ImGui::TextUnformatted(FormatBytes(save.bytes).c_str());
      label(T("Profile"));
      ImGui::TextUnformatted(profile.xuid.c_str());
      if (!achievements_.empty()) {
        int unlocked = 0;
        for (const auto& a : achievements_) unlocked += a.unlocked_filetime != 0;
        label(T("Achievements"));
        ImGui::Text("%d / %d", unlocked, int(achievements_.size()));
      }
      label(T("Folder"));
      ImGui::PushTextWrapPos(0);
      ImGui::TextUnformatted(save.path.string().c_str());
      ImGui::PopTextWrapPos();
      ImGui::SetCursorPosX(label_w);
      for (size_t i = 0; i < save.files.size(); ++i) {
        if (i) ImGui::SameLine();
        ImGui::TextDisabled("%s (%s)%s", save.files[i].first.c_str(),
                            FormatBytes(save.files[i].second).c_str(),
                            i + 1 < save.files.size() ? "," : "");
      }

      ImGui::Spacing();
      ImGui::SetCursorPosX(label_w);
      if (ImGui::Button(T("Open the folder"))) open_folder(save.path);
      ImGui::SameLine();
      // Deleting needs a second click; it can't be undone.
      const std::string key = "delete:" + save.path.string();
      const bool armed = confirm_ == key;
      if (armed) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
      const bool clicked = ImGui::Button(
          (std::string(armed ? T("Click again to delete it for good") : T("Delete")) + "##delete")
              .c_str());
      if (armed) ImGui::PopStyleColor();
      if (clicked && !armed) {
        confirm_ = key;
      } else if (clicked) {
        confirm_.clear();
        std::error_code ec;
        std::filesystem::remove_all(save.path, ec);
        if (!ec) std::filesystem::remove(save.header, ec);
        save_message_ = ec ? T("Could not delete the save.") : T("Save deleted.");
        saves_scanned_ = false;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("The game starts over from the beginning"));
      if (save.has_progress) DrawStory(save.progress, label_w, scale);
      ImGui::PopID();
    }
  }
}

void LauncherDialog::DrawStory(const SaveSummary& progress, float label_w, float scale) {
  if (GameDataValid() && episodes_language_ != settings_.language) {
    episodes_ = ReadEpisodes(settings_.game_data, settings_.language);
    episodes_language_ = settings_.language;
  }
  auto record_of = [&](int id) -> const StoryRecord* {
    for (const auto& r : progress.stories) {
      if (r.id == id) return &r;
    }
    return nullptr;
  };
  auto label = [&](const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(label_w);
  };
  static const char* const kDifficulty[kDifficultyCount] = {"Easy", "Normal", "Hard"};

  SubHeading(HeadingFont(), T("Story"), scale);
  // The hidden True Episode 18 (18_B) only counts once it is reached.
  int total = 0, cleared = 0;
  for (const auto& e : episodes_) {
    const StoryRecord* r = record_of(e.id);
    if (e.number.ends_with("_B") && !(r && r->Reached())) continue;
    ++total;
    cleared += r && r->Cleared();
  }
  if (total) {
    label(T("Episodes"));
    char text[96];
    std::snprintf(text, sizeof(text), T("%d of %d episodes cleared"), cleared, total);
    ProgressBarText(float(cleared) / float(total), ImGui::GetFrameHeight(), text);
  }
  if (progress.difficulty >= 0) {
    label(T("Difficulty"));
    ImGui::TextUnformatted(T(kDifficulty[progress.difficulty]));
  }
  label(T("Battle points"));
  ImGui::Text("%d", progress.battle_points);
  label(T("Enemies defeated"));
  ImGui::Text("%d", progress.enemies_defeated);
  if (episodes_.empty()) {
    ImGui::TextDisabled("%s", T("Set the game folder in System to see the episodes."));
    return;
  }

  ImGui::Spacing();
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
  if (!ImGui::BeginTable("##story", 2 + kDifficultyCount, flags)) return;
  ImGui::TableSetupColumn(T("Episode"), ImGuiTableColumnFlags_WidthFixed, 110 * scale);
  ImGui::TableSetupColumn(T("Title"));
  for (const char* d : kDifficulty) {
    ImGui::TableSetupColumn(T(d), ImGuiTableColumnFlags_WidthFixed, 90 * scale);
  }
  HeadersRow();
  int part = 0;
  auto row = [&](const char* number, const std::string& title, const StoryRecord* r) {
    ImGui::TableNextRow();
    if (r && r->Cleared()) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(kUnlockedRow));
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(number);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(title.c_str());
    for (int d = 0; d < kDifficultyCount; ++d) {
      ImGui::TableNextColumn();
      const std::string rank = r ? r->rank[d] : std::string();
      if (rank.empty()) {
        ImGui::TextDisabled("-");
      } else {
        ImGui::PushFont(HeadingFont());
        ImGui::TextUnformatted(rank.c_str());
        ImGui::PopFont();
      }
    }
  };
  // Only the episodes the player has reached: the titles of later ones are
  // spoilers.
  for (const auto& e : episodes_) {
    const StoryRecord* r = record_of(e.id);
    if (!(r && r->Reached())) continue;
    const bool hidden = e.number.ends_with("_B");
    if (e.part != part) {
      part = e.part;
      static const char* const kRoman[] = {"", "I", "II", "III", "IV"};
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s %s", T("Part"), part >= 0 && part < 5 ? kRoman[part] : "");
    }
    std::string number = e.number.substr(0, e.number.find('_'));
    if (hidden) number += std::string(" (") + T("True") + ")";
    row(number.c_str(), e.title, r);
  }
  // Episodes that aren't in the main list (downloadable ones), once reached.
  bool extras = false;
  for (const auto& r : progress.stories) {
    if (!r.Reached()) continue;
    if (std::any_of(episodes_.begin(), episodes_.end(), [&](const EpisodeInfo& e) { return e.id == r.id; })) {
      continue;
    }
    if (!extras) {
      extras = true;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", T("Extra"));
    }
    row("+", EpisodeTitle(settings_.game_data, settings_.language, r.id), &r);
  }
  ImGui::EndTable();
}

}  // namespace asura::ui
