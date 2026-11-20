// Desktop Linux: what this process is and how it starts itself again or hands
// a path to the desktop. The rest of the POSIX layer is platform_posix.cpp,
// shared with Android (platform_android.cpp).
#include "platform.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <thread>

extern char** environ;

namespace asura::platform {

std::filesystem::path ExePath() {
  std::error_code ec;
  auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : exe;
}

std::filesystem::path LauncherDir() {
  // An AppImage runs from a read-only mount; $APPIMAGE is the file itself.
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) {
    return std::filesystem::path(appimage).parent_path();
  }
  return ExePath().parent_path();
}

std::vector<std::string> ExeArgs() {
  std::vector<std::string> args;
  if (std::ifstream in("/proc/self/cmdline", std::ios::binary); in) {
    for (std::string arg; std::getline(in, arg, '\0');) args.push_back(arg);
  }
  return args;
}

bool CanStartSelf() { return true; }

void ReplaceProcess(const std::filesystem::path& exe, const std::vector<std::string>& args) {
  std::vector<std::string> copy = args;
  std::vector<char*> argv;
  for (auto& a : copy) argv.push_back(a.data());
  argv.push_back(nullptr);
  execv(exe.c_str(), argv.data());
}

bool OpenPath(const std::string& path_or_url) {
  const char* argv[] = {"xdg-open", path_or_url.c_str(), nullptr};
  pid_t pid = -1;
  if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(argv),
                   environ) != 0) {
    return false;
  }
  std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
  return true;
}

}  // namespace asura::platform
