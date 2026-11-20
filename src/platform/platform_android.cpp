// Android: the app is a library inside the activity's process (android/), not
// an executable. There is nothing to start again or exec, so Play runs the
// game in this process (CanStartSelf() is false). The rest of the POSIX layer
// is platform_posix.cpp.
#include "platform.h"

#include <cstdlib>

namespace asura::platform {

// The activity unpacks the APK's assets/ under $ASURA_APP_DIR (its files
// folder) and sets the variable before the library starts: ExePath() stands
// for a program there, so ExePath().parent_path() / "assets" finds them.
std::filesystem::path ExePath() {
  const char* dir = std::getenv("ASURA_APP_DIR");
  if (!dir || !*dir) return {};
  return std::filesystem::path(dir) / "asura_wrath";
}

std::vector<std::string> ExeArgs() { return {"asura_wrath"}; }

bool CanStartSelf() { return false; }

void ReplaceProcess(const std::filesystem::path&, const std::vector<std::string>&) {
  // No exec on Android: returning means "failed", and callers carry on.
}

bool OpenPath(const std::string&) {
  // Folders can't be shown in a file manager by path on Android; links would
  // need an Intent from the activity.
  return false;
}

}  // namespace asura::platform
