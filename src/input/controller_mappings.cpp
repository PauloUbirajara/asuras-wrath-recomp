#include "controller_mappings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_joystick.h>

#include <rex/logging.h>

namespace asura::input {
namespace {

std::mutex g_mutex;
std::filesystem::path g_file;

std::vector<std::string> ReadLines() {
  std::vector<std::string> lines;
  std::ifstream in(g_file);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

bool WriteLines(const std::vector<std::string>& lines) {
  std::error_code ec;
  std::filesystem::create_directories(g_file.parent_path(), ec);
  const auto temp = std::filesystem::path(g_file).concat(".tmp");
  {
    std::ofstream out(temp, std::ios::trunc);
    out << "# Controller mappings from Controls > Setup (SDL gamecontrollerdb format)\n";
    for (const auto& line : lines) {
      if (line.rfind('#', 0) != 0) out << line << '\n';
    }
    if (!out) return false;
  }
  std::filesystem::rename(temp, g_file, ec);
  return !ec;
}

bool IsModel(const std::string& line, const std::string& guid) {
  return line.size() > guid.size() && line.compare(0, guid.size(), guid) == 0 &&
         line[guid.size()] == ',';
}

}  // namespace

void SetControllerMappingsFile(const std::filesystem::path& file) {
  std::lock_guard lock(g_mutex);
  g_file = file;
  SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, file.string().c_str());
}

std::filesystem::path ControllerMappingsFile() {
  std::lock_guard lock(g_mutex);
  return g_file;
}

std::string SavedControllerMapping(const std::string& guid) {
  std::lock_guard lock(g_mutex);
  if (g_file.empty()) return {};
  for (const auto& line : ReadLines()) {
    if (IsModel(line, guid)) return line;
  }
  return {};
}

bool SaveControllerMapping(const std::string& guid, const std::string& mapping) {
  {
    std::lock_guard lock(g_mutex);
    if (g_file.empty()) return false;
    std::vector<std::string> lines;
    for (auto& line : ReadLines()) {
      if (!IsModel(line, guid)) lines.push_back(std::move(line));
    }
    lines.push_back(mapping);
    if (!WriteLines(lines)) {
      REXLOG_WARN("Controller setup: cannot write {}", g_file.string());
      return false;
    }
  }
  if (SDL_AddGamepadMapping(mapping.c_str()) < 0) {
    REXLOG_WARN("Controller setup: SDL refused the mapping {}: {}", mapping, SDL_GetError());
  }
  REXLOG_INFO("Controller setup: saved {}", mapping);
  return true;
}

bool RemoveControllerMapping(const std::string& guid) {
  std::lock_guard lock(g_mutex);
  if (g_file.empty()) return false;
  std::vector<std::string> lines;
  for (auto& line : ReadLines()) {
    if (!IsModel(line, guid)) lines.push_back(std::move(line));
  }
  return WriteLines(lines);
}

namespace {

std::filesystem::path g_android_file;
bool g_android_watch = false;

std::string Trimmed(std::string text) {
  std::replace(text.begin(), text.end(), ',', ' ');
  const size_t first = text.find_first_not_of(' ');
  const size_t last = text.find_last_not_of(' ');
  return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

// The setup line for this controller: by vendor and product, else (no
// USB ids) by name.
void ApplyAndroidSetupTo(SDL_JoystickID id) {
  const int vendor = SDL_GetJoystickVendorForID(id);
  const int product = SDL_GetJoystickProductForID(id);
  const char* sdl_name = SDL_GetJoystickNameForID(id);
  const std::string name = Trimmed(sdl_name ? sdl_name : "");
  std::ifstream in(g_android_file);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t c1 = line.find(','), c2 = line.find(',', c1 + 1), c3 = line.find(',', c2 + 1);
    if (c1 == std::string::npos || c2 == std::string::npos || c3 == std::string::npos) continue;
    const int v = std::atoi(line.substr(0, c1).c_str());
    const int p = std::atoi(line.substr(c1 + 1, c2 - c1 - 1).c_str());
    const std::string n = Trimmed(line.substr(c2 + 1, c3 - c2 - 1));
    const bool match = (v || p) ? v == vendor && p == product : n == name;
    if (!match) continue;
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    const std::string mapping = std::string(guid) + "," + (n.empty() ? "Controller" : n) + "," +
                                line.substr(c3 + 1);
    if (SDL_AddGamepadMapping(mapping.c_str()) < 0) {
      REXLOG_WARN("Controller setup: SDL refused {}: {}", mapping, SDL_GetError());
    } else {
      REXLOG_INFO("Controller setup: {} uses {}", name, mapping);
    }
    return;
  }
}

bool SDLCALL AndroidSetupWatch(void*, SDL_Event* e) {
  if (e->type == SDL_EVENT_JOYSTICK_ADDED) ApplyAndroidSetupTo(e->jdevice.which);
  return true;
}

}  // namespace

void ApplyAndroidControllerSetup(const std::filesystem::path& file) {
  g_android_file = file;
  if (!g_android_watch) {
    g_android_watch = SDL_AddEventWatch(&AndroidSetupWatch, nullptr);
  }
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
    for (int i = 0; i < count; ++i) ApplyAndroidSetupTo(ids[i]);
    SDL_free(ids);
  }
}

}  // namespace asura::input
