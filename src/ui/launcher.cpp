#include "launcher.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

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

#include "game_font.h"

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

struct WindowSize {
  const char* label;
  int width, height;
};
constexpr WindowSize kWindowSizes[] = {
    {"1280 x 720", 0, 0},  // default (the guest video mode)
    {"1600 x 900", 1600, 900},
    {"1920 x 1080", 1920, 1080},
    {"2560 x 1080", 2560, 1080},
    {"2560 x 1440", 2560, 1440},
    {"3440 x 1440", 3440, 1440},
    {"3840 x 2160", 3840, 2160},
};

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

std::filesystem::path CacheRoot() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    return std::filesystem::path(xdg) / "asura_wrath_recomp";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".cache" / "asura_wrath_recomp";
  }
  return {};
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
    else if (key == "keyboard") keyboard = on;
    else if (key == "aspect")
      aspect = value == "fill" ? AspectMode::kFill
               : value == "stretch" ? AspectMode::kStretch
                                    : AspectMode::kFit;
    else if (key == "window") std::sscanf(value.c_str(), "%dx%d", &window_width, &window_height);
    else if (key == "game_data") game_data = value;
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
      << "gpu=" << gpu << "\nvsync=" << vsync << "\nfullscreen=" << fullscreen
      << "\nkeyboard=" << keyboard << "\naspect=" << AspectName(aspect) << "\nwindow="
      << window_width << "x" << window_height << "\ngame_data=" << game_data << "\n";
  for (const auto& [flag, value] : binds) out << "flag." << flag << "=" << value << "\n";
  return bool(out);
}

void ApplyLauncherDefaults(const LauncherSettings& s) {
  SetIfUnset("vulkan_device", std::to_string(s.gpu));
  SetIfUnset("vulkan_allow_present_mode_immediate", s.vsync ? "false" : "true");
  SetIfUnset("vulkan_allow_present_mode_mailbox", s.vsync ? "false" : "true");
  SetIfUnset("fullscreen", s.fullscreen ? "true" : "false");
  SetIfUnset("mnk_mode", s.keyboard ? "true" : "false");
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
}

void UseWindowForLauncher() { SetIfUnset("fullscreen", "false"); }

void ApplyWindowSettings(const LauncherSettings& s) {
  if (s.window_width > 0 && s.window_height > 0) {
    SetLive("window_width", std::to_string(s.window_width));
    SetLive("window_height", std::to_string(s.window_height));
  }
  SetLive("present_letterbox", s.aspect == AspectMode::kStretch ? "false" : "true");
  SetLive("fullscreen", s.fullscreen ? "true" : "false");
}

bool NeedsRestart(const LauncherSettings& before, const LauncherSettings& after) {
  return before.gpu != after.gpu || before.vsync != after.vsync ||
         (before.aspect == AspectMode::kFill) != (after.aspect == AspectMode::kFill);
}

std::filesystem::path ShaderCachePath() {
  if (const char* env = std::getenv("MANTRA_SHADER_CACHE"); env && *env) return env;
  const auto root = CacheRoot();
  return root.empty() ? root : root / "mantra_shaders.mshc";
}

std::filesystem::path AchievementCacheDir() {
  const auto root = CacheRoot();
  return root.empty() ? root : root / "achievements";
}

// --- Shader cache build --------------------------------------------------------

void LauncherDialog::ShaderBuild::Start(const std::filesystem::path& tool,
                                        const std::filesystem::path& game,
                                        const std::filesystem::path& out) {
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
  const char* argv[] = {tool_s.c_str(), "--game", game_s.c_str(), "--out", out_s.c_str(), nullptr};
  pid_t child = -1;
  const int rc = posix_spawn(&child, tool_s.c_str(), &actions, nullptr,
                             const_cast<char* const*>(argv), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    std::lock_guard<std::mutex> lock(mutex);
    last_line = "Could not start " + tool_s + ": " + std::strerror(rc);
    state = kFailed;
    return;
  }
  pid = child;
  state = kRunning;
  {
    std::lock_guard<std::mutex> lock(mutex);
    last_line = "Starting...";
  }
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
    message = "Reading the disc image...";
  }
  worker = std::thread([this, iso, out_dir] {
    auto fail = [this](std::string text) {
      std::lock_guard<std::mutex> lock(mutex);
      message = std::move(text);
      state = kFailed;
    };
    rex::filesystem::DiscImageDevice device("\\Device\\LauncherDisc", iso);
    if (!device.Initialize() || !device.root()) {
      fail("Not an Xbox 360 game disc image: " + iso.filename().string());
      return;
    }
    total_bytes = device.total_file_size();
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
      fail("Cannot create " + out_dir.string() + ": " + ec.message());
      return;
    }
    const auto space = std::filesystem::space(out_dir, ec);
    if (!ec && space.available < total_bytes) {
      fail("Not enough free space: " + FormatBytes(total_bytes) + " needed, " +
           FormatBytes(space.available) + " free");
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
              error = "Write failed: " + dest.string();
              return false;
            }
          }
          return true;
        };
    if (!copy(*device.root(), out_dir)) {
      fail(cancel ? std::string("Cancelled") : error);
      return;
    }
    if (!std::filesystem::is_regular_file(out_dir / "default.xex", ec)) {
      fail("The disc image has no default.xex");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      message = "Done";
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
                               std::function<void(const LauncherSettings&)> on_play)
    : rex::ui::ImGuiDialog(drawer),
      immediate_drawer_(immediate_drawer),
      window_(window),
      font_(font),
      title_font_(title_font ? title_font : font),
      settings_(std::move(settings)),
      settings_path_(std::move(settings_path)),
      user_data_root_(std::move(user_data_root)),
      call_in_ui_thread_deferred_(std::move(call_in_ui_thread_deferred)),
      on_play_(std::move(on_play)) {
  logo_ = LoadImage(GameUiFile("launcher_logo.png"));
  panel_ = LoadImage(GameUiFile("launcher_panel.png"));
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
}

LauncherDialog::~LauncherDialog() {
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  shader_build_.Stop();
  iso_extract_.Stop();
}

LauncherDialog::Image LauncherDialog::LoadImage(const std::filesystem::path& path) {
  Image image;
  std::ifstream file(path, std::ios::binary);
  if (!file || !immediate_drawer_) return image;
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
  int w = 0, h = 0, channels = 0;
  unsigned char* rgba = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &channels, 4);
  if (!rgba) return image;
  image.texture = immediate_drawer_->CreateTexture(
      uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false, rgba);
  stbi_image_free(rgba);
  image.width = float(w);
  image.height = float(h);
  return image;
}

// The list the game wrote on its last start (AsuraWrathApp::
// WriteAchievementCache), with the unlocks the SDK saves in
// <user data>/achievements/<title id>.toml.
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
    if (a.locked_description.empty()) a.locked_description = a.description;
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
      message_ = "That folder has no default.xex.";
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
  if (playing_ || capture_flag_.empty()) return;
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

void LauncherDialog::OnDraw(ImGuiIO& io) {
  if (playing_) return;
  if (iso_extract_.state == IsoExtract::kDone) {
    if (iso_extract_.worker.joinable()) iso_extract_.worker.join();
    iso_extract_.state = IsoExtract::kIdle;
    iso_path_.clear();
    SetGameData(iso_extract_.out.string());
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

bool LauncherDialog::SidebarButton(const char* label, bool selected, float width, float height) {
  ImGui::PushStyleColor(ImGuiCol_Button, selected ? kButtonSelected : kButton);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? kButtonSelected : kButtonHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kButtonSelected);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.90f, 0.86f, 1.0f));
  const bool clicked = ImGui::Button(label, ImVec2(width, height));
  ImGui::PopStyleColor(4);
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
  // Logo (art if present, else the title in the game's font).
  float y = pos.y + 28 * scale;
  if (logo_.texture) {
    const float w = size.x - 24 * scale;
    const float h = w * logo_.height / std::max(logo_.width, 1.0f);
    draw->AddImage(reinterpret_cast<ImTextureID>(logo_.texture.get()),
                   ImVec2(pos.x + 12 * scale, y), ImVec2(pos.x + 12 * scale + w, y + h));
    y += h + 30 * scale;
  } else {
    ImFont* font = title_font_ ? title_font_ : ImGui::GetFont();
    for (const char* text : {"ASURA'S", "WRATH"}) {
      const ImVec2 ts = font->CalcTextSizeA(title_size, FLT_MAX, 0, text);
      draw->AddText(font, title_size, ImVec2(pos.x + (size.x - ts.x) * 0.5f, y),
                    IM_COL32(240, 236, 228, 255), text);
      y += ts.y;
    }
    const char* sub = "RECOMPILED";
    const float sub_size = 12.0f * scale;
    const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(sub_size, FLT_MAX, 0, sub);
    draw->AddText(ImGui::GetFont(), sub_size, ImVec2(pos.x + (size.x - ts.x) * 0.5f, y + 2 * scale),
                  U32(kAccent), sub);
    y += ts.y + 36 * scale;
  }

  ImGui::PushFont(font_, button_size);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.32f, 0.32f, 0.34f, 1.0f));
  const float button_h = 34 * scale, gap = 26 * scale;
  struct Item {
    const char* label;
    Page page;
  };
  for (const Item& item : {Item{"Controls", Page::kControls},
                           Item{"Achievements", Page::kAchievements},
                           Item{"Settings", Page::kSettings}}) {
    ImGui::SetCursorScreenPos(ImVec2(pos.x, y));
    if (SidebarButton(item.label, page_ == item.page, size.x, button_h)) {
      page_ = item.page;
      capture_flag_.clear();
    }
    y += button_h + gap;
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  ImGui::PopFont();

  // Play, at the bottom.
  const bool busy = shader_build_.state == ShaderBuild::kRunning ||
                    iso_extract_.state == IsoExtract::kRunning;
  if (!message_.empty()) {
    ImGui::PushTextWrapPos(pos.x + size.x - 10 * scale);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 10 * scale, pos.y + size.y - 150 * scale));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.55f, 0.40f, 1.0f));
    ImGui::TextUnformatted(message_.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
  }
  ImGui::PushFont(font_, 34.0f * scale);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.06f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.12f));
  ImGui::PushStyleColor(ImGuiCol_Text, busy ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f)
                                            : ImVec4(0.96f, 0.95f, 0.92f, 1.0f));
  ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y - 90 * scale));
  const bool play = ImGui::Button("Play", ImVec2(size.x, 64 * scale)) && !busy;
  ImGui::PopStyleColor(4);
  ImGui::PopFont();
  if (ImGui::IsItemHovered() && busy) ImGui::SetTooltip("Wait for the current task to finish");

  if (play) {
    settings_.game_data = game_data_buffer_;
    if (!GameDataValid()) {
      message_ = "Set the game folder in Settings (it must contain default.xex).";
      page_ = Page::kSettings;
      return;
    }
    capture_flag_.clear();
    SaveSettings();
    playing_ = true;
    // After this frame: the callback may destroy the launcher.
    imgui_drawer()->RemoveDialog(this);
    call_in_ui_thread_deferred_([cb = on_play_, s = settings_] { cb(s); });
  }
}

void LauncherDialog::DrawPanel(const ImVec2& pos, const ImVec2& size, float scale) {
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 end(pos.x + size.x, pos.y + size.y);
  if (panel_.texture) {
    draw->AddImage(reinterpret_cast<ImTextureID>(panel_.texture.get()), pos, end);
  } else {
    const float band = std::floor(size.y * 0.08f);
    draw->AddRectFilled(pos, end, U32(kPanelBand));
    draw->AddRectFilled(ImVec2(pos.x, pos.y + band), ImVec2(end.x, end.y - band), U32(kPanel));
  }

  const float pad = 36 * scale;
  ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, pos.y + pad));
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
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * scale, 5 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * scale, 9 * scale));
  // The game font draws smaller than ImGui's for the same size.
  ImGui::PushFont(font_, (font_ ? 18.0f : 15.0f) * scale);
  if (ImGui::BeginChild("##panel", ImVec2(size.x - 2 * pad, size.y - 2 * pad))) {
    switch (page_) {
      case Page::kControls: DrawControls(scale); break;
      case Page::kAchievements: DrawAchievements(scale); break;
      case Page::kSettings: DrawSettings(scale); break;
    }
  }
  ImGui::EndChild();
  ImGui::PopFont();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(19);
}

namespace {
void Heading(ImFont* font, const char* text, float scale) {
  ImGui::PushFont(font, 26.0f * scale);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::Separator();
  ImGui::Spacing();
}

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
  ImGui::TextDisabled("(set by %s)",
                      rex::cvar::GetFlagSource(flag) == rex::cvar::Source::kCommandLine
                          ? "command line"
                          : "config");
  return true;
}
}  // namespace

void LauncherDialog::DrawControls(float scale) {
  Heading(title_font_, "Controls", scale);
  ImGui::TextWrapped("Gamepads (XInput / SDL) work as on the console.");
  ImGui::Spacing();

  ImGui::BeginDisabled(SetElsewhere("mnk_mode"));
  if (ImGui::Checkbox("Play with the keyboard", &settings_.keyboard)) {
    capture_flag_.clear();
    SaveSettings();
  }
  ImGui::EndDisabled();
  LockedNote("mnk_mode");

  ImGui::BeginDisabled(!settings_.keyboard);
  {
    bool mouse = FlagValue("mnk_mouse") == "true";
    ImGui::BeginDisabled(SetElsewhere("mnk_mouse"));
    if (ImGui::Checkbox("Mouse moves the camera", &mouse)) SetBind("mnk_mouse", mouse ? "true" : "false");
    ImGui::EndDisabled();
    LockedNote("mnk_mouse");

    ImGui::Spacing();
    ImGui::TextDisabled(capture_flag_.empty()
                            ? "Click a key to change it, right-click to clear it."
                            : "Press the new key (combos like Shift+Up work), Esc to cancel.");
    const float table_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.4f;
    const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                        ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##binds", 3, table_flags, ImVec2(0, std::max(table_h, 120 * scale)))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 170 * scale);
      ImGui::TableSetupColumn("Key");
      ImGui::TableSetupColumn("Alternate");
      ImGui::TableHeadersRow();
      for (const BindRow& row : kBindRows) {
        ImGui::PushID(row.flag);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(row.label);
        const bool locked = SetElsewhere(row.flag);
        auto keys = SplitKeys(FlagValue(row.flag));
        keys.resize(std::max<size_t>(keys.size(), 2));
        for (int slot = 0; slot < 2; ++slot) {
          ImGui::TableNextColumn();
          ImGui::PushID(slot);
          const bool capturing = capture_flag_ == row.flag && capture_slot_ == slot;
          std::string text = capturing ? std::string("Press a key...")
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
          if (locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Set by the command line or config file");
          }
          ImGui::PopID();
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (ImGui::Button("Reset keys to defaults")) {
      for (const BindRow& row : kBindRows) SetBind(row.flag, FlagDefault(row.flag));
      capture_flag_.clear();
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("In game: F3 performance, F4 settings, F7 achievements, ` console.");
}

void LauncherDialog::DrawAchievements(float scale) {
  Heading(title_font_, "Achievements", scale);
  if (!achievements_loaded_) LoadAchievements();
  if (achievements_.empty()) {
    ImGui::TextWrapped(
        "The list shows up here after the game has been started once. Achievements unlock "
        "as you play; press F7 in game to see them.");
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
  ImGui::Text("%d / %d unlocked", unlocked, int(achievements_.size()));
  ImGui::SameLine();
  ImGui::TextDisabled("%uG / %uG", earned_gs, total_gs);
  ImGui::ProgressBar(float(unlocked) / float(achievements_.size()), ImVec2(-1, 6 * scale), "");
  ImGui::Spacing();

  const float icon = 52 * scale;
  if (ImGui::BeginChild("##achievements", ImVec2(0, 0))) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (const auto& a : achievements_) {
      const bool is_unlocked = a.unlocked_filetime != 0;
      ImGui::PushID(int(a.id));
      const ImVec2 row_start = ImGui::GetCursorScreenPos();
      draw->ChannelsSplit(2);
      draw->ChannelsSetCurrent(1);
      if (a.icon.texture) {
        ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(a.icon.texture.get()),
                           ImVec2(icon, icon), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                           is_unlocked ? ImVec4(1, 1, 1, 1) : ImVec4(0.45f, 0.42f, 0.40f, 0.55f));
      } else {
        ImGui::Dummy(ImVec2(icon, icon));
      }
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::PushFont(font_, 18.0f * scale);
      ImGui::PushStyleColor(ImGuiCol_Text, is_unlocked ? kPanelText : kPanelTextDim);
      ImGui::TextUnformatted(a.name.c_str());
      ImGui::PopStyleColor();
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::TextDisabled("%uG", a.gamerscore);
      if (is_unlocked && a.unlocked_filetime > 1) {
        // FILETIME: 100 ns ticks since 1601.
        const std::time_t t = std::time_t(a.unlocked_filetime / 10000000ull - 11644473600ull);
        char date[32];
        std::tm tm{};
        localtime_r(&t, &tm);
        std::strftime(date, sizeof(date), "%Y-%m-%d", &tm);
        ImGui::SameLine();
        ImGui::TextDisabled("- unlocked %s", date);
      }
      ImGui::PushStyleColor(ImGuiCol_Text, is_unlocked ? kPanelText : kPanelTextDim);
      ImGui::TextWrapped("%s", (is_unlocked ? a.description : a.locked_description).c_str());
      ImGui::PopStyleColor();
      ImGui::EndGroup();
      const ImVec2 row_end(row_start.x + ImGui::GetContentRegionAvail().x,
                           ImGui::GetItemRectMax().y);
      if (is_unlocked) {
        draw->ChannelsSetCurrent(0);
        draw->AddRectFilled(ImVec2(row_start.x - 4 * scale, row_start.y - 3 * scale),
                            ImVec2(row_end.x, row_end.y + 3 * scale), U32(kUnlockedRow));
      }
      draw->ChannelsMerge();
      ImGui::Spacing();
      ImGui::PopID();
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
  label("Game folder");
  ImGui::SetNextItemWidth(-1);
  ImGui::BeginDisabled(extracting || SetElsewhere("game_data_root"));
  if (ImGui::InputTextWithHint("##game", "extracted game folder, or an .iso",
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
    ImGui::TextDisabled("default.xex found");
  } else if (iso_path_.empty()) {
    ImGui::TextDisabled("Drop the extracted game folder or the disc .iso on this window.");
  }

  if (iso_path_.empty()) return;
  // A disc image: extract it to a folder (the game, the shader cache builder
  // and the UI art all read plain files).
  std::error_code ec;
  ImGui::Spacing();
  label("Disc image");
  const auto size = std::filesystem::file_size(iso_path_, ec);
  ImGui::Text("%s (%s)", iso_path_.filename().string().c_str(), ec ? "?" : FormatBytes(size).c_str());
  label("Extract to");
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
    if (ImGui::Button("Cancel", ImVec2(-1, 0))) iso_extract_.cancel = true;
    ImGui::SetCursorPosX(label_w);
    ImGui::TextDisabled("%s", iso_extract_.Message().c_str());
  } else {
    if (ImGui::Button(state == IsoExtract::kFailed ? "Try again" : "Extract")) {
      iso_extract_.Start(iso_path_, std::filesystem::path(iso_target_buffer_));
    }
    ImGui::SameLine();
    if (ImGui::Button("Dismiss")) iso_path_.clear();
    if (state == IsoExtract::kFailed) {
      ImGui::SetCursorPosX(label_w);
      ImGui::TextDisabled("Failed: %s", iso_extract_.Message().c_str());
    }
  }
}

void LauncherDialog::DrawSettings(float scale) {
  Heading(title_font_, "Settings", scale);
  bool changed = false;
  const float label_w = 170 * scale;
  auto label = [&](const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(label_w);
  };

  DrawGameFolder(label_w, scale);

  SubHeading(title_font_, "Graphics", scale);
  label("Graphics card");
  ImGui::SetNextItemWidth(360 * scale);
  const std::string automatic =
      automatic_gpu_ >= 0 ? "Automatic (" + gpus_[automatic_gpu_].name + ")"
                          : std::string("Automatic");
  const std::string current = settings_.gpu >= 0 && size_t(settings_.gpu) < gpus_.size()
                                  ? gpus_[settings_.gpu].name
                                  : automatic;
  ImGui::BeginDisabled(SetElsewhere("vulkan_device"));
  if (ImGui::BeginCombo("##gpu", current.c_str())) {
    if (ImGui::Selectable(automatic.c_str(), settings_.gpu < 0)) {
      settings_.gpu = -1;
      changed = true;
    }
    for (size_t i = 0; i < gpus_.size(); ++i) {
      if (ImGui::Selectable(gpus_[i].name.c_str(), settings_.gpu == int(i))) {
        settings_.gpu = int(i);
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  LockedNote("vulkan_device");

  label("VSync");
  ImGui::BeginDisabled(SetElsewhere("vulkan_allow_present_mode_immediate"));
  changed |= ImGui::Checkbox("##vsync", &settings_.vsync);
  ImGui::EndDisabled();
  if (!LockedNote("vulkan_allow_present_mode_immediate")) {
    ImGui::SameLine();
    ImGui::TextDisabled("(the game runs at 30 fps either way)");
  }

  SubHeading(title_font_, "Display", scale);
  label("Fullscreen");
  ImGui::BeginDisabled(SetElsewhere("fullscreen"));
  changed |= ImGui::Checkbox("##fullscreen", &settings_.fullscreen);
  ImGui::EndDisabled();
  LockedNote("fullscreen");

  label("Window size");
  ImGui::SetNextItemWidth(200 * scale);
  const char* size_label = "Custom";
  for (const auto& s : kWindowSizes) {
    if (s.width == std::max(settings_.window_width, 0) &&
        s.height == std::max(settings_.window_height, 0)) {
      size_label = s.label;
    }
  }
  ImGui::BeginDisabled(settings_.fullscreen || SetElsewhere("window_width"));
  if (ImGui::BeginCombo("##window", size_label)) {
    for (const auto& s : kWindowSizes) {
      if (ImGui::Selectable(s.label, s.width == settings_.window_width &&
                                         s.height == settings_.window_height)) {
        settings_.window_width = s.width;
        settings_.window_height = s.height;
        changed = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  if (!LockedNote("window_width") && settings_.fullscreen) {
    ImGui::SameLine();
    ImGui::TextDisabled("(fullscreen uses the screen's size)");
  }

  label("Wide screens");
  ImGui::SetNextItemWidth(360 * scale);
  static const char* kAspectLabels[] = {"Fit: 16:9 with black bars",
                                        "Fill: crop the top and bottom",
                                        "Stretch: fill the screen, distorted"};
  int aspect = int(settings_.aspect);
  ImGui::BeginDisabled(SetElsewhere("present_letterbox"));
  if (ImGui::Combo("##aspect", &aspect, kAspectLabels, IM_ARRAYSIZE(kAspectLabels))) {
    settings_.aspect = AspectMode(aspect);
    changed = true;
  }
  ImGui::EndDisabled();
  LockedNote("present_letterbox");
  ImGui::SetCursorPosX(label_w);
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled(
      "The game draws a 16:9 picture at 1280x720 (cutscenes included), scaled to the window. "
      "On ultrawide or 16:10 screens choose how it fills the rest.");
  ImGui::PopTextWrapPos();

  SubHeading(title_font_, "Shaders", scale);
  label("Shader cache");
  const std::filesystem::path cache = ShaderCachePath();
  std::error_code ec;
  const bool have_cache = std::filesystem::is_regular_file(cache, ec);
  const int state = shader_build_.state;
  if (state == ShaderBuild::kRunning) {
    ImGui::TextUnformatted("Building...");
  } else if (have_cache) {
    ImGui::Text("Ready (%.0f MB)", double(std::filesystem::file_size(cache, ec)) / 1048576.0);
  } else {
    ImGui::TextUnformatted("Missing: the game will stutter heavily without it");
  }
  ImGui::SetCursorPosX(label_w);
  const std::filesystem::path tool = rex::filesystem::GetExecutableFolder() / "mantra_shadergen";
  const bool have_tool = std::filesystem::is_regular_file(tool, ec);
  ImGui::BeginDisabled(state == ShaderBuild::kRunning || !have_tool || !GameDataValid() ||
                       iso_extract_.state == IsoExtract::kRunning);
  if (ImGui::Button(have_cache ? "Rebuild" : "Build (about 10 minutes)")) {
    shader_build_.Start(tool, settings_.game_data, cache);
  }
  ImGui::EndDisabled();
  if (state != ShaderBuild::kIdle) {
    ImGui::SetCursorPosX(label_w);
    const std::string line = shader_build_.Line();
    ImGui::TextDisabled("%s%s", state == ShaderBuild::kFailed ? "Failed: " : "", line.c_str());
  }
  if (!have_tool) {
    ImGui::SetCursorPosX(label_w);
    ImGui::TextDisabled("mantra_shadergen not found next to the game");
  }
  ImGui::Spacing();
  ImGui::TextDisabled(
      "Graphics card, VSync and Fill changes restart the game when you press Play.");

  if (changed) SaveSettings();
}

}  // namespace asura::ui
