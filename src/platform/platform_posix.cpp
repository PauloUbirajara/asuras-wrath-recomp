#include "platform.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
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

ExitStatus ToExitStatus(int status) {
  if (WIFEXITED(status)) return {true, WEXITSTATUS(status)};
  return {false, WIFSIGNALED(status) ? WTERMSIG(status) : 0};
}

}  // namespace

std::filesystem::path CacheDir() {
  if (const char* dir = std::getenv(kDataDirVariable); dir && *dir) return dir;
  return DefaultCacheDir();
}

std::filesystem::path DefaultCacheDir() {
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

Process::~Process() {
  if (output_ >= 0) close(output_);
}

bool Process::Start(const std::filesystem::path& exe, const std::vector<std::string>& args,
                    bool capture_output, std::string* error) {
  if (output_ >= 0) close(output_);
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
    const ssize_t n = read(output_, buffer, size);
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

}  // namespace asura::platform
