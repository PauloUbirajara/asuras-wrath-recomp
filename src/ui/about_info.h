// Settings > About: where this build came from and the computer it runs on
// (for bug reports; the launcher copies it as text).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace asura::ui {

inline constexpr const char* kRepositoryUrl =
    "https://github.com/PauloUbirajara/asuras-wrath-recomp";

struct BuildInfo {
  std::string version;      // git describe (a tag, or the commit), "+ changes" if modified
  std::string commit;       // short hash, empty outside a git checkout
  std::string branch;
  std::string commit_date;  // YYYY-MM-DD
  std::string build_date;   // when about_info.cpp was compiled
  std::string compiler;
};
const BuildInfo& GetBuildInfo();

// Label, value: the OS, kernel, desktop session, CPU, memory. Read once.
const std::vector<std::pair<std::string, std::string>>& SystemInfo();

}  // namespace asura::ui
