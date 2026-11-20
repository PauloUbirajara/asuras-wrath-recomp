// Android: the app is a library inside the activity's process (android/), not
// an executable. The Kotlin launcher (LauncherActivity) starts the game in a
// process of its own (GameActivity); there is nothing to exec, so a Play from
// the native launcher runs the game in this process (CanStartSelf() is
// false). The rest of the POSIX layer is platform_posix.cpp.
#include "platform.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace asura::platform {

// The activity unpacks the APK's assets/ under $ASURA_APP_DIR (its files
// folder) and sets the variable before the library starts: ExePath() stands
// for a program there, so ExePath().parent_path() / "assets" finds them.
std::filesystem::path ExePath() {
  const char* dir = std::getenv("ASURA_APP_DIR");
  if (!dir || !*dir) return {};
  return std::filesystem::path(dir) / "asura_wrath";
}

std::filesystem::path LauncherDir() { return ExePath().parent_path(); }

std::vector<std::string> ExeArgs() { return {"asura_wrath"}; }

bool CanStartSelf() { return false; }

void ReplaceProcess(const std::filesystem::path&, const std::vector<std::string>& args) {
  // No exec on Android. The game runs in a process of its own
  // (GameActivity): it leaves a note for LauncherActivity, which starts the
  // game again ("game", with --launcher=false) or stays ("launcher"), and
  // ends. Returns ("failed") only if the note can't be written.
  const char* dir = std::getenv("ASURA_APP_DIR");
  if (!dir || !*dir) return;
  const bool game = std::find(args.begin(), args.end(), "--launcher=false") != args.end();
  std::ofstream note(std::filesystem::path(dir) / ".restart", std::ios::trunc);
  note << (game ? "game" : "launcher") << '\n';
  note.close();
  if (!note) return;
  _exit(0);
}

bool OpenPath(const std::string&) {
  // Folders can't be shown in a file manager by path on Android; links would
  // need an Intent from the activity.
  return false;
}

}  // namespace asura::platform
