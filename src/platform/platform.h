// The few OS services the launcher, the game hooks and mantra need that the
// SDK doesn't wrap: per-user cache folder, this executable, child processes,
// opening a folder or link, environment variables.
//
// One implementation per OS (platform_posix.cpp; Windows to come). Built into
// both the executable and the mantra plugin.
#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace asura::platform {

// The folder for the files the game makes (shader cache, pipelines, UI art,
// movies): $ASURA_DATA_DIR when set (the launcher's "Files folder" setting
// sets it at start, so the game it starts and the mantra plugin see it too),
// else DefaultCacheDir().
std::filesystem::path CacheDir();
// $XDG_CACHE_HOME/asura_wrath_recomp or ~/.cache/asura_wrath_recomp. Empty
// when there is no home folder.
std::filesystem::path DefaultCacheDir();
inline constexpr const char* kDataDirVariable = "ASURA_DATA_DIR";

// This executable, and the arguments it was started with (argv[0] first).
std::filesystem::path ExePath();
std::vector<std::string> ExeArgs();

// Replaces this process with `exe` started with `args` (argv[0] first).
// Only returns if that fails.
void ReplaceProcess(const std::filesystem::path& exe, const std::vector<std::string>& args);

// Sets (or, with nullptr, removes) an environment variable of this process,
// which child processes inherit.
void SetEnv(const char* name, const char* value);

// Opens a folder in the file manager, or a URL in the browser. Returns at once.
bool OpenPath(const std::string& path_or_url);

struct ExitStatus {
  bool normal = false;  // exited by itself (false: killed)
  int code = 0;         // exit code, or the signal that killed it
  bool Ok() const { return normal && code == 0; }
};

// A child process. Not copyable; the destructor neither waits nor kills.
class Process {
 public:
  Process() = default;
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  ~Process();

  // Starts `exe` with `args` (argv[0] first). With `capture_output`, its
  // stdout and stderr go to a pipe that Read returns.
  bool Start(const std::filesystem::path& exe, const std::vector<std::string>& args,
             bool capture_output, std::string* error);
  // Blocks for output; 0 once the process closed it (or without capture).
  long Read(char* buffer, size_t size);
  void Terminate();  // asks it to quit (SIGTERM)
  void Kill();       // ends it now (SIGKILL)
  // The exit status once it ended (then it is no longer Running).
  std::optional<ExitStatus> TryWait();
  ExitStatus Wait();
  bool Running() const { return id_ > 0; }
  int Id() const { return id_; }

 private:
  std::atomic<int> id_{-1};
  int output_ = -1;
};

}  // namespace asura::platform
