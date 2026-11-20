#include "platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <process.h>

namespace asura::platform {
namespace {

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
  return out;
}

std::string Narrow(const wchar_t* s, size_t length) {
  if (!length) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, int(length), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, int(length), out.data(), n, nullptr, nullptr);
  return out;
}

// One argument quoted the way CommandLineToArgvW (and the CRT) splits it.
void AppendQuoted(std::wstring& line, const std::wstring& arg) {
  if (!line.empty()) line += L' ';
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    line += arg;
    return;
  }
  line += L'"';
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      line.append(backslashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      line.append(backslashes * 2 + 1, L'\\');
    } else {
      line.append(backslashes, L'\\');
    }
    line += arg[i];
  }
  line += L'"';
}

std::wstring CommandLine(const std::vector<std::string>& args) {
  std::wstring line;
  for (const auto& a : args) AppendQuoted(line, Widen(a));
  return line;
}

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
  std::filesystem::path result;
  PWSTR path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) result.assign(path);
  CoTaskMemFree(path);
  return result;
}

}  // namespace

std::filesystem::path FilesDir() {
  // _wgetenv: the value may not be representable in the ANSI code page.
  if (const wchar_t* dir = _wgetenv(Widen(kDataDirVariable).c_str()); dir && *dir) {
    return std::filesystem::path(dir);
  }
  return DefaultFilesDir();
}

std::filesystem::path DefaultFilesDir() {
  // The SDK's user folder (rex::filesystem::GetUserFolder) / app name.
  const auto documents = KnownFolder(FOLDERID_Documents);
  return documents.empty() ? documents : documents / "asura_wrath_recomp";
}

std::filesystem::path CacheDir() {
  const auto files = FilesDir();
  return files.empty() ? files : files / "cache";
}

std::filesystem::path LegacyCacheDir() { return {}; }

std::filesystem::path ExePath() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (n == 0) return {};
    if (n < buffer.size()) {
      buffer.resize(n);
      return std::filesystem::path(buffer);
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::filesystem::path LauncherDir() { return ExePath().parent_path(); }

bool CanStartSelf() { return true; }

std::vector<std::string> ExeArgs() {
  std::vector<std::string> args;
  int count = 0;
  if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count)) {
    for (int i = 0; i < count; ++i) args.push_back(Narrow(argv[i], std::wcslen(argv[i])));
    LocalFree(argv);
  }
  return args;
}

// Windows has no exec: start the new process, then end this one.
void ReplaceProcess(const std::filesystem::path& exe, const std::vector<std::string>& args) {
  std::wstring line = CommandLine(args);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                      &startup, &info)) {
    return;
  }
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  ExitProcess(0);
}

void SetEnv(const char* name, const char* value) {
  // Both the CRT's copy (getenv) and the process environment (inherited by
  // CreateProcess) need it.
  const std::wstring wname = Widen(name);
  const std::wstring wvalue = value ? Widen(value) : std::wstring();
  _wputenv_s(wname.c_str(), wvalue.c_str());
  SetEnvironmentVariableW(wname.c_str(), value ? wvalue.c_str() : nullptr);
}

bool OpenPath(const std::string& path_or_url) {
  const auto result = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"open", Widen(path_or_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return result > 32;
}

bool IsWritable(const std::filesystem::path& dir) {
  // _waccess ignores ACLs: try to create a file.
  const std::filesystem::path probe = dir / ".write_test";
  const HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
  CloseHandle(h);
  return true;
}

bool LinkFile(const std::filesystem::path& target, const std::filesystem::path& link,
              std::string* error) {
  if (CreateHardLinkW(link.c_str(), target.c_str(), nullptr)) return true;  // same volume
  std::error_code ec;
  std::filesystem::copy_file(target, link, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec && error) *error = ec.message();
  return !ec;
}

void* LoadedLibrarySymbol(const char* library, const char* symbol) {
  const HMODULE module = GetModuleHandleW(Widen(std::string(library) + ".dll").c_str());
  return module ? reinterpret_cast<void*>(GetProcAddress(module, symbol)) : nullptr;
}

void LowerThreadPriority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
// Child processes inherit the idle class.
void LowerProcessPriority() { SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS); }

namespace {
uint64_t FileTimeNs(const FILETIME& t) {
  return ((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 100;
}
// GetThreadDescription is Windows 10 1607+: looked up, not linked.
std::string ThreadName(HANDLE thread) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const Fn get = reinterpret_cast<Fn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  if (!get) return {};
  PWSTR name = nullptr;
  std::string out;
  if (SUCCEEDED(get(thread, &name)) && name) out = Narrow(name, std::wcslen(name));
  LocalFree(name);
  return out;
}
}  // namespace

uint64_t ThreadCpuNs() {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
  return FileTimeNs(kernel) + FileTimeNs(user);
}

std::vector<ThreadSample> SampleThreads() {
  std::vector<ThreadSample> out;
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return out;
  const DWORD pid = GetCurrentProcessId();
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != pid) continue;
    ThreadSample sample;
    sample.id = int(entry.th32ThreadID);
    if (const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID)) {
      FILETIME created, exited, kernel, user;
      if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
        sample.ns = FileTimeNs(kernel) + FileTimeNs(user);
      }
      sample.name = ThreadName(thread);
      CloseHandle(thread);
    }
    out.push_back(std::move(sample));
  }
  CloseHandle(snapshot);
  return out;
}

uint64_t AvailableMemoryBytes() {
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  return GlobalMemoryStatusEx(&memory) ? memory.ullAvailPhys : 0;
}

int ProcessId() { return int(GetCurrentProcessId()); }

bool ProcessAlive(int pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
  const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return alive;
}

SystemSummary GetSystemSummary() {
  SystemSummary s;
  // GetVersionEx lies without a manifest; RtlGetVersion doesn't.
  using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
  const auto rtl_get_version = reinterpret_cast<Fn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
  OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  if (rtl_get_version && rtl_get_version(&version) == 0) {
    // Windows 11 still reports 10.0; its builds start at 22000.
    const bool eleven = version.dwMajorVersion == 10 && version.dwBuildNumber >= 22000;
    s.os = "Windows " + std::string(eleven ? "11" : std::to_string(version.dwMajorVersion)) +
           " (build " + std::to_string(version.dwBuildNumber) + ")";
  } else {
    s.os = "Windows";
  }
  HKEY key;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                    KEY_READ, &key) == ERROR_SUCCESS) {
    wchar_t name[256] = {};
    DWORD size = sizeof(name) - sizeof(wchar_t);
    if (RegQueryValueExW(key, L"ProcessorNameString", nullptr, nullptr,
                         reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS) {
      std::string cpu = Narrow(name, std::wcslen(name));
      const size_t a = cpu.find_first_not_of(' '), b = cpu.find_last_not_of(' ');
      s.cpu = a == std::string::npos ? std::string() : cpu.substr(a, b - a + 1);
    }
    RegCloseKey(key);
  }
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) s.memory_bytes = memory.ullTotalPhys;
  return s;
}

Process::~Process() {
  if (output_ != -1) CloseHandle(reinterpret_cast<HANDLE>(output_));
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

bool Process::Start(const std::filesystem::path& exe, const std::vector<std::string>& args,
                    bool capture_output, std::string* error) {
  if (output_ != -1) CloseHandle(reinterpret_cast<HANDLE>(output_));
  output_ = -1;
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
  auto fail = [&](DWORD code) {
    if (error) {
      wchar_t* message = nullptr;
      FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                         FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
      *error = message ? Narrow(message, std::wcslen(message)) : "error " + std::to_string(code);
      LocalFree(message);
    }
    return false;
  };
  HANDLE read_end = nullptr, write_end = nullptr;
  if (capture_output) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) return fail(GetLastError());
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);  // only the write end
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  if (capture_output) {
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
  }
  std::wstring line = CommandLine(args);
  PROCESS_INFORMATION info{};
  const BOOL ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr,
                                 capture_output ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup, &info);
  const DWORD code = ok ? 0 : GetLastError();
  if (write_end) CloseHandle(write_end);
  if (!ok) {
    if (read_end) CloseHandle(read_end);
    return fail(code);
  }
  CloseHandle(info.hThread);
  handle_ = info.hProcess;
  output_ = read_end ? reinterpret_cast<intptr_t>(read_end) : -1;
  id_ = int(info.dwProcessId);
  return true;
}

long Process::Read(char* buffer, size_t size) {
  if (output_ == -1) return 0;
  DWORD n = 0;
  if (!ReadFile(reinterpret_cast<HANDLE>(output_), buffer, DWORD(size), &n, nullptr)) return 0;
  return long(n);
}

void Process::Terminate() { Kill(); }

void Process::Kill() {
  if (id_ > 0 && handle_) TerminateProcess(static_cast<HANDLE>(handle_), 1);
}

std::optional<ExitStatus> Process::TryWait() {
  if (id_ <= 0 || !handle_) return std::nullopt;
  if (WaitForSingleObject(static_cast<HANDLE>(handle_), 0) != WAIT_OBJECT_0) return std::nullopt;
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
  id_ = -1;
  return ExitStatus{true, int(code)};
}

ExitStatus Process::Wait() {
  if (id_ <= 0 || !handle_) return {};
  WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
  id_ = -1;
  return ExitStatus{true, int(code)};
}

bool CopyText(const std::string& text, std::string* why) {
  // Windows text uses CRLF line ends.
  std::wstring wide;
  for (const wchar_t c : Widen(text)) {
    if (c == L'\n' && (wide.empty() || wide.back() != L'\r')) wide += L'\r';
    wide += c;
  }
  // Another program may hold the clipboard for a moment: retry briefly.
  bool open = false;
  for (int attempt = 0; attempt < 10 && !(open = OpenClipboard(nullptr)); ++attempt) Sleep(20);
  if (!open) {
    if (why) *why = "OpenClipboard failed (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  bool ok = false;
  EmptyClipboard();
  if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t))) {
    if (void* p = GlobalLock(mem)) {
      std::memcpy(p, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
      GlobalUnlock(mem);
      ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    }
    if (!ok) GlobalFree(mem);  // owned by the clipboard once set
  }
  if (!ok && why) *why = "SetClipboardData failed (error " + std::to_string(GetLastError()) + ")";
  CloseClipboard();
  return ok;
}

}  // namespace asura::platform
