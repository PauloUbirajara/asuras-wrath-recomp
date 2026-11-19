// The few OS services the launcher, the game hooks and mantra need that the
// SDK doesn't wrap: per-user cache folder, this executable, child processes,
// opening a folder or link, environment variables.
//
// One implementation per OS (platform_posix.cpp, platform_win.cpp). Built
// into both the executable and the mantra plugin.
#pragma once

#include <atomic>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace asura::platform {

// The Files folder: everything the game makes goes under it. Saves and the
// profile in it directly, caches in cache/ (CacheDir), logs and traces in
// logs/. $ASURA_DATA_DIR (the launcher's "Files folder" setting; set at start
// for this process, the game it starts and the mantra plugin), else
// DefaultFilesDir().
std::filesystem::path FilesDir();
// The SDK's user folder: $XDG_DATA_HOME/asura_wrath_recomp or
// ~/.local/share/asura_wrath_recomp; Documents\asura_wrath_recomp on Windows.
// launcher.cfg stays there, as it names the Files folder. Empty when there is
// no home folder.
std::filesystem::path DefaultFilesDir();
// FilesDir()/cache: shader cache, pipelines, UI art, movies, the trace trigger.
std::filesystem::path CacheDir();
// Where earlier builds kept the caches ($XDG_CACHE_HOME/asura_wrath_recomp or
// ~/.cache/asura_wrath_recomp), moved into CacheDir() once. Empty on Windows.
std::filesystem::path LegacyCacheDir();
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

// Whether this process can create files in `dir`.
bool IsWritable(const std::filesystem::path& dir);

// Makes `link` show the file `target`: a symlink; on Windows a hard link
// (symlinks need developer mode there), else a copy.
bool LinkFile(const std::filesystem::path& target, const std::filesystem::path& link,
              std::string* error);

// `symbol` of an already loaded library named without prefix or suffix
// ("rexgpu-mantra": librexgpu-mantra.so, rexgpu-mantra.dll); nullptr if it
// isn't loaded. Doesn't load it.
void* LoadedLibrarySymbol(const char* library, const char* symbol);

// Puts the calling thread below normal priority (background work).
void LowerThreadPriority();
// The whole process (and the processes it starts) at background priority.
void LowerProcessPriority();

// CPU time of the calling thread, in nanoseconds.
uint64_t ThreadCpuNs();

// Every thread of this process: id, name, run time so far (ns).
struct ThreadSample {
  int id = 0;
  std::string name;
  uint64_t ns = 0;
};
std::vector<ThreadSample> SampleThreads();

// Memory the system could give this process now (Linux MemAvailable:
// free plus reclaimable cache), in bytes; 0 if unknown.
uint64_t AvailableMemoryBytes();

// This process's id.
int ProcessId();

// Local time of `t` (localtime_r / localtime_s).
inline std::tm LocalTime(std::time_t t) {
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return tm;
}

// For Help > About.
struct SystemSummary {
  std::string os;       // "Ubuntu 25.10", "Windows 11 (build 26100)"
  std::string kernel;   // "6.17.0-40-generic (x86_64)"; empty on Windows
  std::string desktop;  // "GNOME, wayland"; empty on Windows
  std::string cpu;      // model name
  uint64_t memory_bytes = 0;
};
SystemSummary GetSystemSummary();

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
  void Terminate();  // asks it to quit (SIGTERM; on Windows the same as Kill)
  void Kill();       // ends it now (SIGKILL / TerminateProcess)
  // The exit status once it ended (then it is no longer Running).
  std::optional<ExitStatus> TryWait();
  ExitStatus Wait();
  bool Running() const { return id_ > 0; }
  int Id() const { return id_; }

 private:
  std::atomic<int> id_{-1};
  intptr_t output_ = -1;    // the pipe's read end: fd, or HANDLE on Windows
  void* handle_ = nullptr;  // Windows: the process HANDLE
};

}  // namespace asura::platform
