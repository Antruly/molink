// Windows implementation of the core supervisor backend.
//
// The helper is launched by molink-service.exe as SYSTEM in the console
// session. It may find a core already running (the desktop shell starts one
// and then asks the service to start), in which case it waits for that core
// instead of launching a second one that would fight over port 28418. Once the
// core is gone it owns the child: launch, wait, log the exit code, and restart
// with backoff according to supervisor_policy.cpp.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "core_supervisor.h"

#include <windows.h>
#include <tlhelp32.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "diag_log.h"
#include "service_paths.h"

namespace molink {
namespace supervisor {
namespace {

namespace fs = std::filesystem;

/// Paths and Global\ object names for the service that launched this helper.
/// Configured by make_core_supervisor() before the backend is constructed; the
/// helper runs one supervisor per process.
molink::service_paths::paths g_paths;

constexpr wchar_t kCoreExeName[] = L"molink-core.exe";

void append_line(const std::wstring& path, const std::string& line) {
  if (path.empty()) return;
  std::ofstream out(fs::path(path), std::ios::app | std::ios::binary);
  if (out) out << line << "\n";
}

void write_helper_log(const std::string& line) {
  append_line(g_paths.helper_log, line);
}

void write_service_log(const std::string& line) {
  append_line(g_paths.service_log, line);
}

bool enable_privilege(const wchar_t* name) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return false;
  }
  LUID luid{};
  bool ok = false;
  if (LookupPrivilegeValueW(nullptr, name, &luid)) {
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ok = AdjustTokenPrivileges(token, FALSE, &privileges,
                               sizeof(privileges), nullptr, nullptr) != 0 &&
         GetLastError() == ERROR_SUCCESS;
  }
  CloseHandle(token);
  return ok;
}

bool file_exists(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring trim(std::wstring value) {
  const auto is_space = [](wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'"';
  };
  while (!value.empty() && is_space(value.front())) value.erase(value.begin());
  while (!value.empty() && is_space(value.back())) value.pop_back();
  // A UTF-8 BOM is not meaningful in a path file, but strip one anyway.
  if (value.size() >= 3 && value[0] == 0xFEFF) value.erase(value.begin());
  return value;
}

std::wstring read_first_line(const std::wstring& path) {
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) return std::wstring();
  std::string bytes;
  std::getline(in, bytes);
  if (bytes.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                         static_cast<int>(bytes.size()),
                                         nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                      static_cast<int>(bytes.size()), out.data(), needed);
  return trim(out);
}

std::wstring env_var(const wchar_t* name) {
  wchar_t buffer[32768] = {};
  const DWORD length =
      GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
  if (length == 0 || length >= std::size(buffer)) return std::wstring();
  return std::wstring(buffer, length);
}

std::wstring exe_dir() {
  wchar_t buffer[32768] = {};
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
  if (length == 0 || length >= std::size(buffer)) return std::wstring();
  fs::path path(std::wstring(buffer, length));
  return path.parent_path().wstring();
}

std::wstring join_path(const std::wstring& dir, const wchar_t* name) {
  if (dir.empty()) return std::wstring();
  fs::path path(dir);
  path /= name;
  return path.wstring();
}

/// Where molink-core.exe lives. `core.path` is written by the service
/// installer so a package that keeps the core outside the service directory
/// still works; the environment and conventional install locations follow.
std::wstring resolve_core_path() {
  std::vector<std::wstring> candidates;
  const std::wstring hint = read_first_line(g_paths.core_path_hint);
  if (!hint.empty()) candidates.push_back(hint);
  const std::wstring env = trim(env_var(L"MOLINK_CORE"));
  if (!env.empty()) candidates.push_back(env);

  const std::wstring dir = exe_dir();
  if (!dir.empty()) {
    candidates.push_back(join_path(dir, kCoreExeName));
    fs::path parent(dir);
    parent = parent.parent_path();
    candidates.push_back(join_path(parent.wstring(), kCoreExeName));
    fs::path grandparent(parent);
    grandparent = grandparent.parent_path();
    candidates.push_back(join_path(grandparent.wstring(), kCoreExeName));
  }
  for (const wchar_t* base :
       {L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)"}) {
    const std::wstring root = env_var(base);
    if (!root.empty()) {
      candidates.push_back(join_path(join_path(root, L"moLink"),
                                     kCoreExeName));
    }
  }
  for (const std::wstring& candidate : candidates) {
    if (file_exists(candidate)) return candidate;
  }
  return std::wstring();
}

DWORD own_session_id() {
  DWORD session = 0;
  if (ProcessIdToSessionId(GetCurrentProcessId(), &session)) return session;
  return 0;
}

DWORD active_console_session() { return WTSGetActiveConsoleSessionId(); }

/// The first running molink-core.exe, if any. A previous core may still be
/// around for a second after its helper was killed; scanning for the image
/// name catches that as well as a core started by the desktop shell.
DWORD find_existing_core(DWORD exclude_pid, DWORD only_session,
                         bool* scanned = nullptr) {
  if (scanned != nullptr) *scanned = false;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  if (scanned != nullptr) *scanned = true;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  DWORD found = 0;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == 0 ||
          entry.th32ProcessID == GetCurrentProcessId() ||
          entry.th32ProcessID == exclude_pid) {
        continue;
      }
      if (_wcsicmp(entry.szExeFile, kCoreExeName) != 0) continue;
      if (only_session != 0) {
        DWORD session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &session) ||
            session != only_session) {
          continue;
        }
      }
      found = entry.th32ProcessID;
      break;
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return found;
}

/// The helper must own a console for the core to inherit and for its
/// process-group Ctrl+Break to be deliverable. The service launches it with
/// CREATE_NO_WINDOW, which already yields a hidden console; allocate one only
/// if some other launcher did not.
bool ensure_hidden_console() {
  static int state = -1;  // -1 unknown, 0 unavailable, 1 ready
  if (state >= 0) return state == 1;
  if (GetConsoleCP() != 0) {
    state = 1;
    return true;
  }
  if (!AllocConsole()) {
    write_helper_log("AllocConsole failed error=" +
                     std::to_string(GetLastError()));
    state = 0;
    return false;
  }
  if (HWND window = GetConsoleWindow()) ShowWindow(window, SW_HIDE);
  write_helper_log("allocated a hidden console for core control events");
  state = 1;
  return true;
}

unsigned long long process_creation_ticks(HANDLE process) {
  if (process == nullptr) return 0;
  FILETIME creation{};
  FILETIME exit_time{};
  FILETIME kernel{};
  FILETIME user{};
  if (!GetProcessTimes(process, &creation, &exit_time, &kernel, &user)) {
    return 0;
  }
  return (static_cast<unsigned long long>(creation.dwHighDateTime) << 32) |
         creation.dwLowDateTime;
}

/// The service uses this to avoid killing an unrelated process after Windows
/// reuses the pid, so store the process creation time next to the pid.
void write_core_pid(DWORD pid, HANDLE process) {
  if (g_paths.core_pid.empty()) return;
  std::ofstream out(fs::path(g_paths.core_pid), std::ios::trunc);
  if (out) out << pid << " " << process_creation_ticks(process) << "\n";
}

void clear_core_pid() {
  if (g_paths.core_pid.empty()) return;
  DeleteFileW(g_paths.core_pid.c_str());
}

class core_supervisor_backend final : public backend {
 public:
  core_supervisor_backend(std::atomic<bool>& running,
                          const restart_policy& policy)
      : running_(running),
        policy_(policy),
        session_(own_session_id()),
        stop_event_(OpenEventW(SYNCHRONIZE, FALSE,
                               g_paths.helper_stop_event.c_str())),
        detach_event_(OpenEventW(SYNCHRONIZE, FALSE,
                                 g_paths.helper_detach_event.c_str())) {}

  ~core_supervisor_backend() override {
    close_core();
    if (external_ != nullptr) CloseHandle(external_);
    if (stop_event_ != nullptr) CloseHandle(stop_event_);
    if (detach_event_ != nullptr) CloseHandle(detach_event_);
    release_instance();
  }

  bool acquire_instance() override {
    // A short wait covers the service's stop-old-helper/start-new-helper
    // hand-off, where the previous mutex may still be closing.
    for (int attempt = 0; attempt < 10; ++attempt) {
      instance_ = CreateMutexW(nullptr, TRUE,
                               g_paths.supervisor_mutex.c_str());
      if (instance_ == nullptr) return false;
      if (GetLastError() != ERROR_ALREADY_EXISTS) return true;
      CloseHandle(instance_);
      instance_ = nullptr;
      if (stopping()) return false;
      Sleep(200);
    }
    write_helper_log("core supervisor mutex is held by another helper");
    return false;
  }

  void release_instance() override {
    if (instance_ == nullptr) return;
    ReleaseMutex(instance_);
    CloseHandle(instance_);
    instance_ = nullptr;
  }

  bool external_core_running() override {
    // `pid_` is our previous core after a crash. Its process object can still
    // be listed until close_core, and adopting it would count the same exit
    // twice.
    bool scanned = false;
    external_pid_ = find_existing_core(pid_, session_, &scanned);
    if (!scanned) {
      // A failed snapshot must not be read as "no core"; wait for the next
      // poll instead of launching a duplicate.
      write_helper_log("core scan failed, waiting");
      return true;
    }
    if (external_pid_ == 0) return false;
    external_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                                PROCESS_TERMINATE,
                            FALSE, external_pid_);
    if (external_ == nullptr) {
      // Try again with query rights only: we may not be allowed to terminate
      // it, but the service runs with more privileges and can use core.pid.
      external_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, external_pid_);
      write_helper_log("cannot fully open core pid=" +
                       std::to_string(external_pid_) +
                       " error=" + std::to_string(GetLastError()));
    }
    // Record it too: a session switch with a dead helper uses core.pid to
    // clean up a core this helper was only watching.
    write_core_pid(external_pid_, external_);
    write_helper_log("core already running pid=" + std::to_string(external_pid_));
    return true;
  }

  bool wait_external_core(int timeout_ms, int& exit_code) override {
    const DWORD timeout = static_cast<DWORD>(timeout_ms);
    if (external_ == nullptr) {
      // The first scan could not open the core. Re-scan; once it is gone,
      // treat the disappearance as an abnormal exit so we launch our own.
      bool scanned = false;
      const DWORD still = find_existing_core(pid_, session_, &scanned);
      if (!scanned) {
        Sleep(timeout);
        return false;
      }
      if (still == 0) {
        exit_code = 1;
        // Keep the pid excluded from the next scan until the process object
        // is fully reaped; otherwise the same exit can be counted twice.
        pid_ = external_pid_;
        external_pid_ = 0;
        clear_core_pid();
        return true;
      }
      // The snapshot recovered and the core is still there: adopt it.
      external_pid_ = still;
      external_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                                  PROCESS_TERMINATE,
                              FALSE, still);
      if (external_ == nullptr) {
        external_ = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, still);
      }
      write_core_pid(external_pid_, external_);
      if (external_ == nullptr) {
        Sleep(timeout);
        return false;
      }
    }
    if (WaitForSingleObject(external_, timeout) != WAIT_OBJECT_0) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(external_, &code)) code = 1;
    CloseHandle(external_);
    external_ = nullptr;
    pid_ = external_pid_;
    external_pid_ = 0;
    clear_core_pid();
    exit_code = static_cast<int>(code);
    return true;
  }

  bool start_core(std::string& error, unsigned long& pid) override {
    close_core();
    clear_core_pid();
    if (core_path_.empty()) {
      core_path_ = resolve_core_path();
      if (core_path_.empty()) {
        write_helper_log("molink-core.exe not found");
        error = "molink-core.exe not found";
        return false;
      }
      write_helper_log("core path=" + utf8(core_path_));
    }

    std::wstring command = L"\"" + core_path_ + L"\"";
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    const std::wstring workdir = fs::path(core_path_).parent_path().wstring();
    // The core inherits the helper's console. The helper is started with
    // CREATE_NO_WINDOW, so that console is hidden; inheriting it (rather than
    // giving the core its own) is what lets GenerateConsoleCtrlEvent reach
    // the core's process group for a graceful stop.
    const bool console_ready = ensure_hidden_console();
    // Without a console to inherit, suppress the core's console window; the
    // graceful Ctrl+Break stop is then unavailable and stop_core terminates.
    const DWORD creation =
        CREATE_NEW_PROCESS_GROUP | (console_ready ? 0 : CREATE_NO_WINDOW);

    HANDLE user_token = console_user_token();
    void* environment = nullptr;
    DWORD flags = creation;
    BOOL created = FALSE;
    if (user_token != nullptr) {
      if (CreateEnvironmentBlock(&environment, user_token, FALSE)) {
        flags |= CREATE_UNICODE_ENVIRONMENT;
      }
      created = CreateProcessAsUserW(
          user_token, core_path_.c_str(), command_line.data(), nullptr,
          nullptr, FALSE, flags, environment,
          workdir.empty() ? nullptr : workdir.c_str(), &startup, &process_);
      if (!created) {
        write_helper_log("CreateProcessAsUser failed error=" +
                   std::to_string(GetLastError()) + ", using helper token");
      }
    }
    if (!created) {
      if (environment != nullptr) {
        DestroyEnvironmentBlock(environment);
        environment = nullptr;
      }
      flags = creation;
      created = CreateProcessW(
          core_path_.c_str(), command_line.data(), nullptr, nullptr, FALSE,
          flags, nullptr, workdir.empty() ? nullptr : workdir.c_str(),
          &startup, &process_);
    }
    if (user_token != nullptr) CloseHandle(user_token);
    if (environment != nullptr) DestroyEnvironmentBlock(environment);

    if (!created) {
      error = "CreateProcess failed: " + std::to_string(GetLastError());
      process_ = PROCESS_INFORMATION{};
      return false;
    }
    CloseHandle(process_.hThread);
    process_.hThread = nullptr;
    pid_ = process_.dwProcessId;
    write_core_pid(pid_, process_.hProcess);
    pid = pid_;
    return true;
  }

  bool wait_core(int timeout_ms, int& exit_code) override {
    if (process_.hProcess == nullptr) {
      exit_code = 0;
      return true;
    }
    if (WaitForSingleObject(process_.hProcess,
                            static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) {
      return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process_.hProcess, &code)) code = 1;
    exit_code = static_cast<int>(code);
    clear_core_pid();
    return true;
  }

  int stop_core() override {
    if (process_.hProcess != nullptr) {
      const int code = stop_handle(process_.hProcess, pid_, true);
      // Leave core.pid behind if the core refused to die: the service uses it
      // to clean up before the new session's helper starts.
      if (WaitForSingleObject(process_.hProcess, 0) == WAIT_OBJECT_0) {
        clear_core_pid();
      }
      close_core();
      return code;
    }
    if (external_ != nullptr) {
      // A core we did not launch (the desktop shell's) must still be stopped
      // when the console moves away from this helper.
      const int code = stop_handle(external_, external_pid_, false);
      if (WaitForSingleObject(external_, 0) == WAIT_OBJECT_0) {
        clear_core_pid();
      }
      CloseHandle(external_);
      external_ = nullptr;
      external_pid_ = 0;
      return code;
    }
    if (external_pid_ != 0) {
      // We could not open it, so we cannot stop it. Leave core.pid for the
      // service, which validates the creation time before terminating.
      write_helper_log("cannot stop unopened core pid=" +
                       std::to_string(external_pid_));
      return 0;
    }
    clear_core_pid();
    return 0;
  }

  bool stopping() override {
    if (!running_.load()) return true;
    if (stop_event_ != nullptr &&
        WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
      if (!stop_requested_) {
        stop_requested_ = true;
        write_helper_log("helper stop requested");
      }
      return true;
    }
    if (detach_requested()) {
      if (!detach_logged_) {
        detach_logged_ = true;
        write_helper_log("helper detach requested");
      }
      return true;
    }
    // A console switch is handled by the service, which disables
    // SERVICE_CONTROL_STOP before it asks us to stop. Acting on the session
    // change first would let the old core's shutdown stop the service before
    // the new helper can be launched. Give the service a few seconds to send
    // its stop event, then fall back so a dead service cannot strand the old
    // helper in the old session.
    return session_changed();
  }

  bool detach_requested() override {
    return detach_event_ != nullptr &&
           WaitForSingleObject(detach_event_, 0) == WAIT_OBJECT_0;
  }

  void sleep_interruptible(int64_t ms) override {
    const int64_t step =
        std::max<int64_t>(policy_.poll_interval_ms, static_cast<int64_t>(50));
    for (int64_t slept = 0; slept < ms; slept += step) {
      if (stopping()) return;
      const int64_t remaining = ms - slept;
      // Parenthesised: windows.h defines min/max as macros, and `std::min(`
      // then parses as `std::(` under MSVC (C2589).
      Sleep(static_cast<DWORD>((std::min)(step, remaining)));
    }
  }

  void log_service(const std::string& line) override {
    write_service_log(line);
  }

  void log_app_exit(int code, const std::string& reason) override {
    std::vector<diag_field> fields;
    fields.push_back({"reason", reason});
    fields.push_back({"code", std::to_string(code)});
    fields.push_back({"supervisor", "helper"});
    molink::diag().log(molink::diag_level::info, "app.exit", std::string(),
                       fields);
  }

 private:
  static std::string utf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
  }

  HANDLE console_user_token() {
    if (session_ == 0 || session_ == 0xFFFFFFFF) return nullptr;
    HANDLE token = nullptr;
    if (!WTSQueryUserToken(session_, &token)) {
      write_helper_log("no console user token in session " +
                 std::to_string(session_) +
                 " error=" + std::to_string(GetLastError()) +
                 ", running the core as the helper");
      return nullptr;
    }
    return token;
  }


  int stop_handle(HANDLE process, DWORD pid, bool same_console) {
    bool signaled = false;
    if (same_console) {
      // The core inherited this process's console, so the process group is on
      // a console we are already attached to.
      signaled = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid) != 0;
    } else {
      // An adopted core may have its own console. We are stopping for good,
      // so detaching from ours to borrow theirs is safe.
      FreeConsole();
      if (AttachConsole(pid)) {
        signaled = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid) != 0;
        FreeConsole();
      } else {
        signaled = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid) != 0;
      }
    }
    write_helper_log(std::string("core stop signal pid=") + std::to_string(pid) +
                     (signaled ? " sent" : " unavailable"));

    DWORD wait =
        WaitForSingleObject(process, static_cast<DWORD>(policy_.stop_grace_ms));
    if (wait != WAIT_OBJECT_0) {
      write_helper_log("core did not stop in " +
                       std::to_string(policy_.stop_grace_ms) +
                       "ms, terminating pid=" + std::to_string(pid));
      if (!TerminateProcess(process, 1)) {
        write_helper_log("TerminateProcess failed error=" +
                         std::to_string(GetLastError()));
      }
      WaitForSingleObject(process, 2000);
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process, &code)) code = 1;
    return static_cast<int>(code);
  }

  void close_core() {
    if (process_.hThread != nullptr) {
      CloseHandle(process_.hThread);
      process_.hThread = nullptr;
    }
    if (process_.hProcess != nullptr) {
      CloseHandle(process_.hProcess);
      process_.hProcess = nullptr;
    }
    process_.dwProcessId = 0;
    pid_ = 0;
  }

  bool session_changed() {
    if (session_ == 0 || session_ == 0xFFFFFFFF) return false;
    const DWORD active = active_console_session();
    if (active == 0xFFFFFFFF || active == session_) return false;
    const ULONGLONG now = GetTickCount64();
    if (!session_change_logged_) {
      session_change_logged_ = true;
      session_change_tick_ = now;
      write_helper_log(
          "session changed " + std::to_string(session_) + " -> " +
          std::to_string(active) +
          (stop_event_ != nullptr ? ", waiting for the service"
                                  : ", stopping core"));
    }
    if (stop_event_ != nullptr && now - session_change_tick_ < 5000) {
      return false;
    }
    return true;
  }

  std::atomic<bool>& running_;
  restart_policy policy_;
  DWORD session_ = 0;
  HANDLE stop_event_ = nullptr;
  HANDLE detach_event_ = nullptr;
  HANDLE instance_ = nullptr;
  HANDLE external_ = nullptr;
  DWORD external_pid_ = 0;
  PROCESS_INFORMATION process_{};
  DWORD pid_ = 0;
  std::wstring core_path_;
  bool stop_requested_ = false;
  bool detach_logged_ = false;
  bool session_change_logged_ = false;
  ULONGLONG session_change_tick_ = 0;
};

}  // namespace

std::unique_ptr<backend> make_core_supervisor(std::atomic<bool>& running,
                                              const restart_policy& policy,
                                              std::wstring service_name) {
  // Resolve the service's directory, logs and object names before the backend
  // opens its events and mutex. molink-helper's main() has already logged the
  // service.name/data_dir/supervisor line after parsing --name.
  g_paths =
      molink::service_paths::resolve(env_var(L"ProgramData"), service_name);
  // These are already held by LocalSystem; enabling them lets the helper ask
  // WTS for the console user's token and create the global objects the
  // service and helper share.
  enable_privilege(L"SeTcbPrivilege");
  enable_privilege(L"SeAssignPrimaryTokenPrivilege");
  enable_privilege(L"SeIncreaseQuotaPrivilege");
  enable_privilege(L"SeCreateGlobalPrivilege");
  // Needed to stop a core started by another account or session.
  enable_privilege(L"SeDebugPrivilege");
  return std::make_unique<core_supervisor_backend>(running, policy);
}

}  // namespace supervisor
}  // namespace molink
