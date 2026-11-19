#include "about_info.h"

#include <sys/sysinfo.h>
#include <sys/utsname.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

#include "build_info.h"  // generated (tools/build_info.cmake)

namespace asura::ui {
namespace {

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

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

}  // namespace

const BuildInfo& GetBuildInfo() {
  static const BuildInfo info = [] {
    BuildInfo b;
    b.commit = ASURA_GIT_COMMIT;
    b.branch = ASURA_GIT_BRANCH;
    b.commit_date = ASURA_GIT_COMMIT_DATE;
    b.version = *ASURA_GIT_DESCRIBE ? ASURA_GIT_DESCRIBE : "unknown";
    if (ASURA_GIT_DIRTY) b.version += " + changes";
    // __DATE__ is "Mmm dd yyyy".
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::string date = __DATE__;
    int month = 0;
    for (int i = 0; i < 12; ++i) {
      if (date.compare(0, 3, kMonths[i]) == 0) month = i + 1;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%s-%02d-%02d %.5s", date.substr(7, 4).c_str(), month,
                  std::atoi(date.substr(4, 2).c_str()), __TIME__);
    b.build_date = text;
#if defined(__clang__)
    b.compiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
    b.compiler = "GCC " __VERSION__;
#endif
    b.compiler = Trim(b.compiler);
    return b;
  }();
  return info;
}

const std::vector<std::pair<std::string, std::string>>& SystemInfo() {
  static const auto info = [] {
    std::vector<std::pair<std::string, std::string>> rows;
    std::string os = ReadField("/etc/os-release", "PRETTY_NAME", '=');
    if (os.empty()) os = "Linux";
    rows.emplace_back("OS", os);
    if (utsname u; uname(&u) == 0) {
      rows.emplace_back("Kernel", std::string(u.release) + " (" + u.machine + ")");
    }
    std::string session = Env("XDG_CURRENT_DESKTOP");
    const std::string type = Env("XDG_SESSION_TYPE");
    if (!type.empty()) session += (session.empty() ? "" : ", ") + type;
    if (!session.empty()) rows.emplace_back("Desktop", session);
    std::string cpu = ReadField("/proc/cpuinfo", "model name", ':');
    if (cpu.empty()) cpu = "unknown";
    if (const unsigned n = std::thread::hardware_concurrency()) {
      cpu += " (" + std::to_string(n) + " threads)";
    }
    rows.emplace_back("CPU", cpu);
    if (struct sysinfo s; sysinfo(&s) == 0) {
      const double gib = double(s.totalram) * s.mem_unit / (1024.0 * 1024.0 * 1024.0);
      char text[32];
      std::snprintf(text, sizeof(text), "%.1f GiB", gib);
      rows.emplace_back("Memory", text);
    }
    return rows;
  }();
  return info;
}

}  // namespace asura::ui
