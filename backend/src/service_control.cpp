#include "service_control.h"

#include <cstdint>
#include <iterator>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#endif

namespace molink {
namespace {

#if defined(_WIN32)
constexpr wchar_t kServiceName[] = L"moLinkSession";

std::wstring service_executable() {
  wchar_t path[MAX_PATH * 4] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(
                                                    std::size(path)));
  if (length == 0) return std::wstring();
  std::wstring exe(path, length);
  const std::size_t slash = exe.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return std::wstring();
  return exe.substr(0, slash + 1) + L"molink-service.exe";
}

bool run_elevated(const wchar_t* verb, const std::string& args,
                  std::string& err) {
  const std::wstring exe = service_executable();
  if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    err = "molink-service.exe is not installed next to the core";
    return false;
  }
  const std::wstring wide_args(args.begin(), args.end());
  const HINSTANCE result = ShellExecuteW(nullptr, verb, exe.c_str(),
                                         wide_args.c_str(), nullptr, SW_HIDE);
  if (reinterpret_cast<std::intptr_t>(result) <= 32) {
    err = "could not start the elevated service helper";
    return false;
  }
  return true;
}
#elif defined(__linux__)
namespace fs = std::filesystem;

std::string env_value(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

std::string read_exe(pid_t pid) {
  if (pid <= 0) return std::string();
  const std::string link = "/proc/" + std::to_string(pid) + "/exe";
  char buffer[4096] = {};
  const ssize_t length = ::readlink(link.c_str(), buffer, sizeof(buffer) - 1);
  if (length <= 0) return std::string();
  return std::string(buffer, static_cast<std::size_t>(length));
}

bool executable(const std::string& path) {
  return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

std::string find_shell() {
  const std::string explicit_path = env_value("MOLINK_SHELL");
  if (executable(explicit_path)) return explicit_path;
  // The core is spawned by the desktop shell, so its parent is the shell.
  const std::string parent = read_exe(::getppid());
  if (executable(parent) && parent.find("molink-core") == std::string::npos) {
    return parent;
  }
  const std::string path = env_value("PATH");
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find(':', start);
    const std::string dir =
        path.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
    if (!dir.empty()) {
      const std::string candidate = dir + "/moLink";
      if (executable(candidate)) return candidate;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  for (const char* candidate : {"/usr/bin/moLink", "/usr/local/bin/moLink",
                                "/opt/molink/bin/moLink"}) {
    if (executable(candidate)) return candidate;
  }
  return std::string();
}

std::string config_home() {
  std::string dir = env_value("XDG_CONFIG_HOME");
  if (!dir.empty()) return dir;
  const std::string home = env_value("HOME");
  return home.empty() ? std::string() : home + "/.config";
}

std::string unit_path() {
  const std::string dir = config_home();
  if (dir.empty()) return std::string();
  return dir + "/systemd/user/molink.service";
}

int run_command(const std::vector<std::string>& args) {
  if (args.empty()) return 127;
  const pid_t pid = ::fork();
  if (pid < 0) return 127;
  if (pid == 0) {
    const int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      ::close(null_fd);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

std::string escape_unit_value(std::string value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    if (c == '%') {
      out += "%%";
    } else if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n' || c == '\r') {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string unit_text(const std::string& shell) {
  std::string unit =
      "[Unit]\n"
      "Description=moLink desktop\n"
      "After=graphical-session.target\n"
      "PartOf=graphical-session.target\n"
      "\n"
      "[Service]\n"
      "Type=simple\n";
  for (const char* name :
       {"DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_SESSION_TYPE",
        "XDG_CURRENT_DESKTOP", "GDK_BACKEND"}) {
    const std::string value = env_value(name);
    if (value.empty()) continue;
    unit += std::string("Environment=\"") + name + "=" +
            escape_unit_value(value) + "\"\n";
  }
  unit += "ExecStart=\"" + escape_unit_value(shell) + "\"\n";
  unit +=
      "Restart=no\n"
      "\n"
      "[Install]\n"
      "WantedBy=graphical-session.target\n";
  return unit;
}

bool write_unit(const std::string& path, const std::string& text,
                std::string& err) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    err = "cannot write " + path;
    return false;
  }
  out << text;
  out.close();
  if (!out) {
    err = "cannot write " + path;
    return false;
  }
  return true;
}
#endif

}  // namespace

bool running_in_console_session() {
#if defined(_WIN32)
  DWORD session = 0;
  const DWORD console = WTSGetActiveConsoleSessionId();
  return ProcessIdToSessionId(GetCurrentProcessId(), &session) != 0 &&
         session == console;
#else
  return false;
#endif
}

service_status_info query_session_service() {
  service_status_info info;
#if defined(_WIN32)
  info.available = true;
  info.platform = "windows";
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) {
    info.error = "OpenSCManager failed";
    return info;
  }
  SC_HANDLE service = OpenServiceW(manager, kServiceName,
                                   SERVICE_QUERY_STATUS);
  if (service == nullptr) {
    const DWORD code = GetLastError();
    if (code != ERROR_SERVICE_DOES_NOT_EXIST) {
      info.error = "OpenService failed";
    }
    CloseServiceHandle(manager);
    return info;
  }
  SERVICE_STATUS status{};
  if (QueryServiceStatus(service, &status)) {
    info.installed = true;
    info.running = status.dwCurrentState == SERVICE_RUNNING;
  } else {
    info.error = "QueryServiceStatus failed";
  }
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
#elif defined(__linux__)
  info.available = true;
  info.platform = "linux";
  info.autostart = true;
  const std::string path = unit_path();
  info.installed = !path.empty() && fs::exists(path);
  info.running =
      run_command({"systemctl", "--user", "is-active", "--quiet",
                   "molink.service"}) == 0;
#endif
  return info;
}

bool start_session_service(std::string& err) {
#if defined(_WIN32)
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) {
    err = "OpenSCManager failed";
    return false;
  }
  SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_START);
  if (service == nullptr) {
    err = "service is not installed";
    CloseServiceHandle(manager);
    return false;
  }
  BOOL ok = FALSE;
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (StartServiceW(service, 0, nullptr) ||
        GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
      ok = TRUE;
      break;
    }
    Sleep(300);
  }
  if (!ok) err = "StartService failed";
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return ok != 0;
#elif defined(__linux__)
  const std::string path = unit_path();
  if (path.empty() || !fs::exists(path)) {
    err = "service is not installed";
    return false;
  }
  const int status =
      run_command({"systemctl", "--user", "start", "molink.service"});
  if (status == 127) {
    err = "systemctl --user is unavailable";
  } else if (status != 0) {
    err = "systemctl --user start failed";
  }
  return status == 0;
#else
  (void)err;
  return false;
#endif
}

bool stop_session_service(std::string& err) {
#if defined(_WIN32)
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) {
    err = "OpenSCManager failed";
    return false;
  }
  SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_STOP);
  if (service == nullptr) {
    err = "service is not installed";
    CloseServiceHandle(manager);
    return false;
  }
  SERVICE_STATUS status{};
  const BOOL ok = ControlService(service, SERVICE_CONTROL_STOP, &status);
  if (!ok && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
    err = "ControlService(stop) failed";
  }
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return ok != 0 || GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
#elif defined(__linux__)
  const std::string path = unit_path();
  if (path.empty() || !fs::exists(path)) {
    err = "service is not installed";
    return false;
  }
  const int status =
      run_command({"systemctl", "--user", "stop", "molink.service"});
  if (status == 127) {
    err = "systemctl --user is unavailable";
  } else if (status != 0) {
    err = "systemctl --user stop failed";
  }
  return status == 0;
#else
  (void)err;
  return false;
#endif
}

bool restart_session_service(std::string& err) {
#if defined(_WIN32)
  stop_session_service(err);
  for (int i = 0; i < 30; ++i) {
    if (!query_session_service().running) break;
    Sleep(100);
  }
  return start_session_service(err);
#elif defined(__linux__)
  const std::string path = unit_path();
  if (path.empty() || !fs::exists(path)) {
    err = "service is not installed";
    return false;
  }
  const int status =
      run_command({"systemctl", "--user", "restart", "molink.service"});
  if (status == 127) {
    err = "systemctl --user is unavailable";
  } else if (status != 0) {
    err = "systemctl --user restart failed";
  }
  return status == 0;
#else
  (void)err;
  return false;
#endif
}

bool send_secure_attention(std::string& err) {
#if defined(_WIN32)
  HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\molink-session",
                            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    err = "the session service is not running";
    return false;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
  const char command[] = "sas";
  DWORD written = 0;
  if (!WriteFile(pipe, command, 3, &written, nullptr)) {
    err = "could not ask the service for Ctrl+Alt+Del";
    CloseHandle(pipe);
    return false;
  }
  char buffer[256] = {};
  DWORD read = 0;
  ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr);
  CloseHandle(pipe);
  if (read == 0) {
    err = "the session service did not reply";
    return false;
  }
  const std::string reply(buffer, read);
  if (reply.find("\"ok\":true") == std::string::npos) {
    err = "Ctrl+Alt+Del was rejected: " + reply;
    return false;
  }
  return true;
#else
  (void)err;
  return false;
#endif
}

bool install_session_service(std::string& err) {
#if defined(_WIN32)
  return run_elevated(L"runas", "--install", err);
#elif defined(__linux__)
  const std::string path = unit_path();
  if (path.empty()) {
    err = "cannot resolve the user configuration directory";
    return false;
  }
  const std::string shell = find_shell();
  if (shell.empty()) {
    err = "cannot find the moLink desktop shell (set MOLINK_SHELL)";
    return false;
  }
  if (!write_unit(path, unit_text(shell), err)) return false;
  if (run_command({"systemctl", "--user", "daemon-reload"}) != 0) {
    err = "systemctl --user daemon-reload failed";
    return false;
  }
  if (run_command({"systemctl", "--user", "enable", "molink.service"}) != 0) {
    err = "systemctl --user enable failed";
    return false;
  }
  return true;
#else
  (void)err;
  return false;
#endif
}

bool uninstall_session_service(std::string& err) {
#if defined(_WIN32)
  return run_elevated(L"runas", "--uninstall", err);
#elif defined(__linux__)
  const std::string path = unit_path();
  if (path.empty()) {
    err = "cannot resolve the user configuration directory";
    return false;
  }
  // Disable the login autostart but do not stop the current window: the panel
  // asking for this runs inside it, and disabling is what "uninstall" means
  // here. "Stop" remains the explicit way to end a service-managed window.
  run_command({"systemctl", "--user", "disable", "molink.service"});
  std::error_code ec;
  fs::remove(path, ec);
  if (ec) {
    err = "cannot remove " + path;
    return false;
  }
  run_command({"systemctl", "--user", "daemon-reload"});
  return true;
#else
  (void)err;
  return false;
#endif
}

}  // namespace molink
