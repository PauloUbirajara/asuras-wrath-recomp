#include "platform.h"

#include <dlfcn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <thread>

extern char** environ;

namespace asura::platform {
namespace {

std::vector<char*> Argv(std::vector<std::string>& args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  return argv;
}

std::string Trim(std::string s) {
  const size_t a = s.find_first_not_of(" \t\"");
  const size_t b = s.find_last_not_of(" \t\"\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The value of the first "key<sep>value" line in `path`.
std::string ReadField(const char* path, const std::string& key, char sep) {
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    const size_t at = line.find(sep);
    if (at != std::string::npos && Trim(line.substr(0, at)) == key) {
      return Trim(line.substr(at + 1));
    }
  }
  return {};
}

ExitStatus ToExitStatus(int status) {
  if (WIFEXITED(status)) return {true, WEXITSTATUS(status)};
  return {false, WIFSIGNALED(status) ? WTERMSIG(status) : 0};
}

}  // namespace

std::filesystem::path FilesDir() {
  if (const char* dir = std::getenv(kDataDirVariable); dir && *dir) return dir;
  return DefaultFilesDir();
}

std::filesystem::path DefaultFilesDir() {
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    return std::filesystem::path(xdg) / "asura_wrath_recomp";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".local" / "share" / "asura_wrath_recomp";
  }
  return {};
}

std::filesystem::path CacheDir() {
  const auto files = FilesDir();
  return files.empty() ? files : files / "cache";
}

std::filesystem::path LegacyCacheDir() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
    return std::filesystem::path(xdg) / "asura_wrath_recomp";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::filesystem::path(home) / ".cache" / "asura_wrath_recomp";
  }
  return {};
}

std::filesystem::path ExePath() {
  std::error_code ec;
  auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : exe;
}

std::vector<std::string> ExeArgs() {
  std::vector<std::string> args;
  if (std::ifstream in("/proc/self/cmdline", std::ios::binary); in) {
    for (std::string arg; std::getline(in, arg, '\0');) args.push_back(arg);
  }
  return args;
}

void ReplaceProcess(const std::filesystem::path& exe, const std::vector<std::string>& args) {
  std::vector<std::string> copy = args;
  execv(exe.c_str(), Argv(copy).data());
}

void SetEnv(const char* name, const char* value) {
  if (value) {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
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

bool IsWritable(const std::filesystem::path& dir) { return access(dir.c_str(), W_OK) == 0; }

bool LinkFile(const std::filesystem::path& target, const std::filesystem::path& link,
              std::string* error) {
  std::error_code ec;
  std::filesystem::create_symlink(target, link, ec);
  if (ec && error) *error = ec.message();
  return !ec;
}

void* LoadedLibrarySymbol(const char* library, const char* symbol) {
  const std::string name = std::string("lib") + library + ".so";
  // RTLD_NOLOAD only finds it (the SDK loaded it RTLD_LOCAL).
  void* lib = dlopen(name.c_str(), RTLD_NOW | RTLD_NOLOAD);
  return lib ? dlsym(lib, symbol) : nullptr;
}

void LowerThreadPriority() { setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 10); }
// Linux: the calling thread; threads and processes it starts inherit it.
void LowerProcessPriority() { setpriority(PRIO_PROCESS, 0, 15); }

uint64_t ThreadCpuNs() {
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

std::vector<ThreadSample> SampleThreads() {
  std::vector<ThreadSample> out;
  std::error_code ec;
  for (const auto& dir : std::filesystem::directory_iterator("/proc/self/task", ec)) {
    ThreadSample sample;
    sample.id = std::atoi(dir.path().filename().c_str());
    if (std::FILE* f = std::fopen((dir.path() / "schedstat").c_str(), "r")) {
      unsigned long long ns = 0;
      if (std::fscanf(f, "%llu", &ns) == 1) sample.ns = ns;
      std::fclose(f);
    }
    if (std::FILE* f = std::fopen((dir.path() / "comm").c_str(), "r")) {
      char name[64] = {};
      if (std::fgets(name, sizeof(name), f)) {
        sample.name = name;
        while (!sample.name.empty() && sample.name.back() == '\n') sample.name.pop_back();
      }
      std::fclose(f);
    }
    out.push_back(std::move(sample));
  }
  return out;
}

uint64_t AvailableMemoryBytes() {
  unsigned long long kb = 0;
  if (std::FILE* meminfo = std::fopen("/proc/meminfo", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), meminfo)) {
      if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
    }
    std::fclose(meminfo);
  }
  return uint64_t(kb) << 10;
}

int ProcessId() { return int(getpid()); }

SystemSummary GetSystemSummary() {
  SystemSummary s;
  s.os = ReadField("/etc/os-release", "PRETTY_NAME", '=');
  if (s.os.empty()) s.os = "Linux";
  if (utsname u; uname(&u) == 0) s.kernel = std::string(u.release) + " (" + u.machine + ")";
  const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
  const char* type = std::getenv("XDG_SESSION_TYPE");
  s.desktop = desktop ? desktop : "";
  if (type && *type) s.desktop += (s.desktop.empty() ? "" : ", ") + std::string(type);
  s.cpu = ReadField("/proc/cpuinfo", "model name", ':');
  if (struct sysinfo info; sysinfo(&info) == 0) {
    s.memory_bytes = uint64_t(info.totalram) * info.mem_unit;
  }
  return s;
}

Process::~Process() {
  if (output_ >= 0) close(int(output_));
}

bool Process::Start(const std::filesystem::path& exe, const std::vector<std::string>& args,
                    bool capture_output, std::string* error) {
  if (output_ >= 0) close(int(output_));
  output_ = -1;
  int fds[2] = {-1, -1};
  if (capture_output && pipe(fds) != 0) {
    if (error) *error = std::strerror(errno);
    return false;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (capture_output) {
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
  }
  std::vector<std::string> copy = args;
  pid_t child = -1;
  const int rc = posix_spawn(&child, exe.c_str(), &actions, nullptr, Argv(copy).data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (capture_output) close(fds[1]);
  if (rc != 0) {
    if (capture_output) close(fds[0]);
    if (error) *error = std::strerror(rc);
    return false;
  }
  output_ = fds[0];
  id_ = child;
  return true;
}

long Process::Read(char* buffer, size_t size) {
  if (output_ < 0) return 0;
  for (;;) {
    const ssize_t n = read(int(output_), buffer, size);
    if (n < 0 && errno == EINTR) continue;
    return n > 0 ? long(n) : 0;
  }
}

void Process::Terminate() {
  if (const int id = id_; id > 0) kill(id, SIGTERM);
}

void Process::Kill() {
  if (const int id = id_; id > 0) kill(id, SIGKILL);
}

std::optional<ExitStatus> Process::TryWait() {
  const int id = id_;
  if (id <= 0) return std::nullopt;
  int status = 0;
  const pid_t r = waitpid(id, &status, WNOHANG);
  if (r == 0) return std::nullopt;
  id_ = -1;
  return r == id ? ToExitStatus(status) : ExitStatus{};
}

ExitStatus Process::Wait() {
  const int id = id_;
  if (id <= 0) return {};
  int status = 0;
  pid_t r;
  do {
    r = waitpid(id, &status, 0);
  } while (r < 0 && errno == EINTR);
  id_ = -1;
  return r == id ? ToExitStatus(status) : ExitStatus{};
}

bool CopyText(const std::string&, std::string* why) {
  if (why) *why = "no native clipboard";  // X11/Wayland: SDL's, which has the window
  return false;
}

}  // namespace asura::platform
