// moLink-host: the host-only Windows shell.
//
// This executable is deliberately small: one fixed Direct2D 1.0 window, one
// loopback HTTP poller, and a tray icon. It never opens a remote-desktop
// window, a file panel, a chat or a terminal. The core does all session work;
// this UI only reads /api/device + /api/state + /api/settings and writes the
// few settings the host-only product exposes.
//
// Threading model
//   * UI thread: window messages, Direct2D drawing, all user actions.
//   * Worker thread: finds/spawns the core and polls the three endpoints,
//     publishing an immutable host_snapshot. The UI never blocks on HTTP
//     during painting; action buttons do one short synchronous request.
//
// Lifecycle (the part the product spec is strict about)
//   * Closing the window while "moLinkHost" is installed hides the UI to the
//     tray; the service keeps the core running.
//   * Closing it while the service is not installed terminates the core this
//     shell spawned (an adopted core is never killed) and exits.
//   * The tray "退出" entry exits the UI only and, when the service is
//     installed, says so before doing it.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "host_settings.h"
#include "host_state.h"
#include "service_ctl.h"

// windows.h renames the D2D DrawText method and its call together (the macro
// is active while d2d1.h is parsed), so call DrawText as written; do not
// #undef it.

namespace {

using molink::hostui::access_switches;
using molink::hostui::device_info;
using molink::hostui::host_snapshot;
using molink::hostui::service_state;
using molink::hostui::service_status;
using molink::hostui::settings_info;
using molink::hostui::state_info;

constexpr wchar_t kWindowClass[] = L"moLinkHostShellWindow";
constexpr wchar_t kWindowTitle[] = L"moLink 被控端";
constexpr wchar_t kRunValueName[] = L"moLinkHost";
constexpr UINT kSnapshotMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;
constexpr UINT_PTR kUiTimer = 1;
constexpr UINT kTrayId = 1;
constexpr int kMenuShow = 1001;
constexpr int kMenuExit = 1002;
constexpr int kClientWidth = 520;
constexpr int kClientHeight = 420;
constexpr int kDefaultPort = 28418;
constexpr int kDefaultUdpPort = 28422;

/// How to talk to the adopted/started core. Kept out of host_state.h because
/// it is transport detail, not part of the parsed API model.
struct core_conn {
  int port = 0;
  bool tls = false;
  std::string token;
  std::string client_token;
};

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

/// Minimal owning COM pointer. WRL is available in the Windows SDK but not in
/// every MinGW header set, and the shell only needs Release/public/put.
template <typename T>
class com_ptr {
 public:
  com_ptr() = default;
  ~com_ptr() { reset(); }
  com_ptr(const com_ptr&) = delete;
  com_ptr& operator=(const com_ptr&) = delete;

  T* get() const { return pointer_; }
  T* operator->() const { return pointer_; }
  explicit operator bool() const { return pointer_ != nullptr; }
  T** put() {
    reset();
    return &pointer_;
  }
  void reset(T* pointer = nullptr) {
    if (pointer_ != nullptr) pointer_->Release();
    pointer_ = pointer;
  }

 private:
  T* pointer_ = nullptr;
};

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int length = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), length);
  return out;
}

std::string wide_to_utf8(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (length <= 0) return std::string();
  std::string out(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), length, nullptr, nullptr);
  return out;
}

std::wstring widen_ascii(const std::string& text) {
  return std::wstring(text.begin(), text.end());
}

std::wstring environment(const wchar_t* name) {
  wchar_t value[MAX_PATH * 4] = {};
  const DWORD length = GetEnvironmentVariableW(
      name, value, static_cast<DWORD>(std::size(value)));
  if (length == 0 || length >= std::size(value)) return std::wstring();
  return std::wstring(value, length);
}

bool file_exists(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool directory_exists(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring module_path() {
  wchar_t path[MAX_PATH * 4] = {};
  const DWORD length = GetModuleFileNameW(
      nullptr, path, static_cast<DWORD>(std::size(path)));
  if (length == 0 || length >= std::size(path)) return std::wstring();
  return std::wstring(path, length);
}

std::wstring module_directory() {
  const std::wstring full = module_path();
  const std::size_t slash = full.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring()
                                     : full.substr(0, slash);
}

/// Create every component of @p path (the Win7-compatible way; SHCreateDirectory
/// pulls in shlobj and is not worth it for two call sites).
bool ensure_directory(const std::wstring& path) {
  if (path.empty()) return false;
  std::wstring current;
  current.reserve(path.size());
  for (wchar_t c : path) {
    current += c;
    if ((c == L'\\' || c == L'/') && current.size() > 3) {
      CreateDirectoryW(current.c_str(), nullptr);
    }
  }
  CreateDirectoryW(path.c_str(), nullptr);
  return directory_exists(path);
}

std::wstring config_directory() {
  const std::wstring override_dir = environment(L"MOLINK_CONFIG_DIR");
  if (!override_dir.empty()) return override_dir;
  const std::wstring appdata = environment(L"APPDATA");
  return appdata.empty() ? std::wstring() : appdata + L"\\moLink";
}

/// The same per-user log directory the core's diag log uses.
std::wstring log_directory() {
  const std::wstring program_data = environment(L"ProgramData");
  if (!program_data.empty() &&
      directory_exists(program_data + L"\\moLink")) {
    return program_data + L"\\moLink\\logs";
  }
  const std::wstring local = environment(L"LOCALAPPDATA");
  if (!local.empty()) return local + L"\\moLink\\logs";
  const std::wstring roaming = environment(L"APPDATA");
  if (!roaming.empty()) return roaming + L"\\moLink\\logs";
  return program_data.empty() ? std::wstring()
                              : program_data + L"\\moLink\\logs";
}

/// tcp_port persisted by the core (written by POST /api/network).
int persisted_core_port() {
  const std::wstring directory = config_directory();
  if (directory.empty()) return 0;
  const std::wstring path = directory + L"\\tcp_port";
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return 0;
  char buffer[32] = {};
  DWORD read = 0;
  ReadFile(file, buffer, static_cast<DWORD>(std::size(buffer) - 1), &read,
           nullptr);
  CloseHandle(file);
  if (read == 0) return 0;
  const int port = std::atoi(buffer);
  return port > 0 && port <= 65535 ? port : 0;
}

bool environment_port(int& port) {
  const std::wstring value = environment(L"MOLINK_CORE_PORT");
  if (value.empty()) return false;
  wchar_t* end = nullptr;
  const long parsed = std::wcstol(value.c_str(), &end, 10);
  if (end == value.c_str() || parsed < 1 || parsed > 65535) return false;
  port = static_cast<int>(parsed);
  return true;
}

std::wstring find_core_executable() {
  const std::wstring override_path = environment(L"MOLINK_CORE");
  if (file_exists(override_path)) return override_path;

  std::vector<std::wstring> candidates;
  const std::wstring directory = module_directory();
  if (!directory.empty()) {
    candidates.push_back(directory + L"\\molink-core.exe");
    candidates.push_back(directory + L"\\..\\lib\\moLink\\molink-core.exe");
    candidates.push_back(directory +
                         L"\\..\\..\\..\\backend\\build\\molink-core.exe");
  }
  candidates.push_back(L"backend\\build\\molink-core.exe");
  candidates.push_back(L"..\\backend\\build\\molink-core.exe");
  for (const std::wstring& candidate : candidates) {
    if (file_exists(candidate)) return candidate;
  }
  return std::wstring();
}

// ---------------------------------------------------------------------------
// Loopback HTTP (WinHTTP)
// ---------------------------------------------------------------------------

struct http_result {
  bool ok = false;
  int status = 0;
  std::string body;
  std::string error;
};

http_result http_request(const core_conn& connection, const wchar_t* method,
                         const std::wstring& path) {
  http_result result;
  if (connection.port <= 0) {
    result.error = "本机核心未运行";
    return result;
  }
  HINTERNET session = WinHttpOpen(
      L"moLink-host/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) {
    result.error = "WinHTTP 初始化失败，错误码 " +
                   std::to_string(GetLastError()) + "）";
    return result;
  }
  // Loopback: stay well under the UI's one-second refresh budget.
  WinHttpSetTimeouts(session, 600, 600, 800, 1000);
  HINTERNET connection_handle = WinHttpConnect(
      session, L"127.0.0.1", static_cast<INTERNET_PORT>(connection.port), 0);
  HINTERNET request = connection_handle != nullptr
                          ? WinHttpOpenRequest(
                                connection_handle, method, path.c_str(),
                                nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                connection.tls ? WINHTTP_FLAG_SECURE : 0)
                          : nullptr;
  if (request != nullptr && connection.tls) {
    // A loopback probe of the core's self-signed certificate is not a trust
    // decision; the UI is local to the machine.
    DWORD ignore = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                   SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                   SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                   SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &ignore,
                     sizeof(ignore));
  }

  std::wstring headers;
  if (!connection.token.empty()) {
    headers += L"Authorization: Bearer " + widen_ascii(connection.token) +
               L"\r\n";
  }
  if (!connection.client_token.empty()) {
    headers += L"X-MoLink-Client: " + widen_ascii(connection.client_token) +
               L"\r\n";
  }

  bool received = false;
  if (request != nullptr) {
    const BOOL sent = WinHttpSendRequest(
        request,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA,
        0, 0, 0);
    if (sent && WinHttpReceiveResponse(request, nullptr)) received = true;
  }
  if (received) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE |
                                WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
      result.status = static_cast<int>(status);
    }
    for (;;) {
      char buffer[4096];
      DWORD read = 0;
      if (!WinHttpReadData(request, buffer,
                           static_cast<DWORD>(std::size(buffer)), &read) ||
          read == 0) {
        break;
      }
      result.body.append(buffer, read);
      if (result.body.size() > 2u * 1024u * 1024u) break;
    }
    result.ok = result.status >= 200 && result.status < 300;
    if (!result.ok) result.error = "HTTP " + std::to_string(result.status);
  } else {
    result.error = "WinHTTP 请求失败，错误码 " +
                   std::to_string(GetLastError()) + "）";
  }

  if (request != nullptr) WinHttpCloseHandle(request);
  if (connection_handle != nullptr) WinHttpCloseHandle(connection_handle);
  WinHttpCloseHandle(session);
  return result;
}

bool probe_core_at(int port, bool tls, device_info& device,
                   std::string& error) {
  core_conn connection;
  connection.port = port;
  connection.tls = tls;
  const http_result result =
      http_request(connection, L"GET", L"/api/device");
  if (!result.ok) {
    error = result.error;
    return false;
  }
  return molink::hostui::parse_device(result.body, device, error);
}

// ---------------------------------------------------------------------------
// Mutable shared state
// ---------------------------------------------------------------------------

std::mutex g_snapshot_mutex;
host_snapshot g_snapshot;

std::mutex g_connection_mutex;
core_conn g_connection;

std::mutex g_spawn_mutex;
HANDLE g_spawned_process = nullptr;
DWORD g_spawned_pid = 0;

std::atomic<bool> g_running{true};
std::atomic<bool> g_poll_now{false};
std::atomic<bool> g_start_core_request{false};
std::atomic<bool> g_stop_core_request{false};
bool g_attach_only = false;
int g_attach_port = 0;
std::string g_core_error;
int g_host_only_enforced_pid = 0;
ULONGLONG g_host_only_retry_at = 0;

std::thread g_worker;
HWND g_window = nullptr;
bool g_tray_added = false;

core_conn get_connection() {
  std::lock_guard<std::mutex> lock(g_connection_mutex);
  return g_connection;
}

void set_connection(const core_conn& connection) {
  std::lock_guard<std::mutex> lock(g_connection_mutex);
  g_connection = connection;
}

void clear_connection() {
  std::lock_guard<std::mutex> lock(g_connection_mutex);
  g_connection = core_conn{};
}

host_snapshot get_snapshot() {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  return g_snapshot;
}

std::vector<int> probe_ports() {
  std::vector<int> ports;
  auto add = [&ports](int port) {
    if (port <= 0 || port > 65535) return;
    for (int existing : ports) {
      if (existing == port) return;
    }
    ports.push_back(port);
  };
  const core_conn current = get_connection();
  add(current.port);
  add(g_attach_port);
  add(persisted_core_port());
  add(kDefaultPort);
  return ports;
}

/// Probe every candidate port, plain HTTP first, then TLS (encrypt_lan). On
/// success fills the connection's tokens and the parsed /api/device view.
bool find_core(core_conn& connection, device_info& device) {
  for (int port : probe_ports()) {
    for (int secure = 0; secure < 2; ++secure) {
      device_info candidate;
      std::string error;
      if (!probe_core_at(port, secure == 1, candidate, error)) continue;
      connection.port = candidate.port > 0 ? candidate.port : port;
      connection.tls = candidate.tls || secure == 1;
      connection.token = candidate.token;
      connection.client_token = candidate.client_token;
      device = std::move(candidate);
      return true;
    }
  }
  return false;
}

bool spawned_core_alive() {
  std::lock_guard<std::mutex> lock(g_spawn_mutex);
  if (g_spawned_process == nullptr) return false;
  if (WaitForSingleObject(g_spawned_process, 0) == WAIT_TIMEOUT) return true;
  CloseHandle(g_spawned_process);
  g_spawned_process = nullptr;
  g_spawned_pid = 0;
  return false;
}

void close_spawned_handle() {
  std::lock_guard<std::mutex> lock(g_spawn_mutex);
  if (g_spawned_process != nullptr) {
    CloseHandle(g_spawned_process);
    g_spawned_process = nullptr;
  }
  g_spawned_pid = 0;
}

void terminate_spawned_core() {
  std::lock_guard<std::mutex> lock(g_spawn_mutex);
  if (g_spawned_process != nullptr) {
    TerminateProcess(g_spawned_process, 0);
    WaitForSingleObject(g_spawned_process, 3000);
    CloseHandle(g_spawned_process);
    g_spawned_process = nullptr;
  }
  g_spawned_pid = 0;
}

bool spawn_core(std::string& error) {
  const std::wstring executable = find_core_executable();
  if (executable.empty()) {
    error = "未找到核心程序，可设置环境变量 MOLINK_CORE";
    return false;
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  // The core's libuvcpp logger writes continuously. Append it to the same
  // per-user log directory the diag log uses, so "打开日志目录" shows it.
  const std::wstring directory = log_directory();
  ensure_directory(directory);
  HANDLE log = INVALID_HANDLE_VALUE;
  if (!directory.empty()) {
    log = CreateFileW((directory + L"\\host-shell-core.log").c_str(),
                      FILE_APPEND_DATA | SYNCHRONIZE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  HANDLE null_input =
      CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  &attributes, OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  if (log != INVALID_HANDLE_VALUE && null_input != INVALID_HANDLE_VALUE) {
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input;
    startup.hStdOutput = log;
    startup.hStdError = log;
  }

  // No --print-ready: the ready line carries the per-launch tokens, and this
  // shell reads them from /api/device instead of persisting them in the log.
  // --parent-pid still makes the core leave if this UI dies.
  std::wstring command = L"\"" + executable +
                         L"\" --parent-pid " +
                         std::to_wstring(GetCurrentProcessId());
  if (g_attach_port > 0) {
    command += L" --port " + std::to_wstring(g_attach_port);
  }
  std::vector<wchar_t> command_line(command.begin(), command.end());
  command_line.push_back(L'\0');

  PROCESS_INFORMATION process{};
  const BOOL started = CreateProcessW(
      nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
      nullptr, nullptr, &startup, &process);
  const DWORD code = GetLastError();
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
  if (!started) {
    error = "无法启动核心程序，错误码 " + std::to_string(code);
    return false;
  }
  CloseHandle(process.hThread);
  {
    std::lock_guard<std::mutex> lock(g_spawn_mutex);
    if (g_spawned_process != nullptr) CloseHandle(g_spawned_process);
    g_spawned_process = process.hProcess;
    g_spawned_pid = process.dwProcessId;
  }
  g_core_error.clear();
  return true;
}

bool kill_process_by_pid(int pid, std::string& error) {
  if (pid <= 0 || pid == static_cast<int>(GetCurrentProcessId())) {
    error = "无效的核心进程号";
    return false;
  }
  HANDLE process =
      OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                  static_cast<DWORD>(pid));
  if (process == nullptr) {
    const DWORD code = GetLastError();
    if (code == ERROR_INVALID_PARAMETER) return true;  // already gone
    error = "无法结束核心进程，错误码 " + std::to_string(code);
    return false;
  }
  TerminateProcess(process, 0);
  WaitForSingleObject(process, 3000);
  CloseHandle(process);
  return true;
}

/// Pull one string field out of a small API response so an error from the
/// core can be shown verbatim instead of a bare HTTP status.
std::string extract_json_string(const std::string& body, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  std::size_t pos = body.find(needle);
  if (pos == std::string::npos) return std::string();
  pos += needle.size();
  while (pos < body.size() &&
         std::isspace(static_cast<unsigned char>(body[pos])) != 0) {
    ++pos;
  }
  if (pos >= body.size() || body[pos] != '"') return std::string();
  ++pos;
  std::string out;
  while (pos < body.size()) {
    const char c = body[pos++];
    if (c == '"') break;
    if (c == '\\' && pos < body.size()) {
      const char escaped = body[pos++];
      switch (escaped) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        default: out += escaped; break;
      }
    } else {
      out += c;
    }
  }
  return out;
}

/// One /api/settings write. The core's handler takes query parameters; the
/// UI's two security switches are projected onto its single access_policy.
bool post_api(const std::wstring& path, std::string& error,
              std::string* body = nullptr) {
  core_conn connection = get_connection();
  if (connection.port <= 0) {
    error = "本机核心未运行";
    return false;
  }
  http_result result = http_request(connection, L"POST", path);
  // The token changes when the core restarts between polls; refresh once.
  if (!result.ok && (result.status == 401 || result.status == 403)) {
    device_info device;
    std::string probe_error;
    if (probe_core_at(connection.port, connection.tls, device,
                      probe_error)) {
      connection.token = device.token;
      connection.client_token = device.client_token;
      set_connection(connection);
      result = http_request(connection, L"POST", path);
    }
  }
  if (!result.ok) {
    error = result.error;
    if (!result.body.empty()) {
      // The core answers with {"code":"...","message":"..."}; prefer message.
      const std::string message = extract_json_string(result.body, "message");
      if (!message.empty()) error = message;
    }
    return false;
  }
  if (body != nullptr) *body = result.body;
  error.clear();
  return true;
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------

enum class target {
  none,
  copy_code,
  port_edit,
  remote_toggle,
  discovery_toggle,
  mode_single,
  mode_multi,
  long_edit,
  long_save,
  input_toggle,
  clipboard_toggle,
  install_service,
  uninstall_service,
  start_service,
  stop_service,
  autostart,
  open_logs,
};

struct layout_rects {
  RECT copy_button{};
  RECT port_edit{};
  RECT remote_toggle{};
  RECT discovery_toggle{};
  RECT mode_single{};
  RECT mode_multi{};
  RECT long_edit{};
  RECT long_save{};
  RECT input_toggle{};
  RECT clipboard_toggle{};
  RECT install_button{};
  RECT uninstall_button{};
  RECT start_button{};
  RECT stop_button{};
  RECT autostart_box{};
  RECT autostart_hit{};
  RECT logs_button{};
};

RECT rect_of(int left, int top, int right, int bottom) {
  RECT rect{};
  rect.left = left;
  rect.top = top;
  rect.right = right;
  rect.bottom = bottom;
  return rect;
}

layout_rects compute_layout() {
  layout_rects layout;
  layout.copy_button = rect_of(416, 53, 498, 75);
  layout.port_edit = rect_of(92, 125, 156, 147);
  layout.remote_toggle = rect_of(442, 150, 498, 172);
  layout.discovery_toggle = rect_of(442, 175, 498, 197);
  layout.mode_single = rect_of(402, 225, 446, 247);
  layout.mode_multi = rect_of(452, 225, 496, 247);
  layout.long_edit = rect_of(92, 251, 324, 273);
  layout.long_save = rect_of(330, 251, 386, 273);
  layout.input_toggle = rect_of(442, 277, 498, 299);
  layout.clipboard_toggle = rect_of(442, 303, 498, 325);
  layout.install_button = rect_of(22, 338, 92, 362);
  layout.uninstall_button = rect_of(96, 338, 166, 362);
  layout.start_button = rect_of(170, 338, 218, 362);
  layout.stop_button = rect_of(222, 338, 270, 362);
  layout.autostart_box = rect_of(280, 343, 294, 357);
  layout.autostart_hit = rect_of(276, 334, 360, 362);
  layout.logs_button = rect_of(366, 338, 456, 362);
  return layout;
}

target hit_test(const layout_rects& layout, POINT point) {
  if (PtInRect(&layout.copy_button, point)) return target::copy_code;
  if (PtInRect(&layout.port_edit, point)) return target::port_edit;
  if (PtInRect(&layout.remote_toggle, point)) return target::remote_toggle;
  if (PtInRect(&layout.discovery_toggle, point)) {
    return target::discovery_toggle;
  }
  if (PtInRect(&layout.mode_single, point)) return target::mode_single;
  if (PtInRect(&layout.mode_multi, point)) return target::mode_multi;
  if (PtInRect(&layout.long_edit, point)) return target::long_edit;
  if (PtInRect(&layout.long_save, point)) return target::long_save;
  if (PtInRect(&layout.input_toggle, point)) return target::input_toggle;
  if (PtInRect(&layout.clipboard_toggle, point)) {
    return target::clipboard_toggle;
  }
  if (PtInRect(&layout.install_button, point)) return target::install_service;
  if (PtInRect(&layout.uninstall_button, point)) {
    return target::uninstall_service;
  }
  if (PtInRect(&layout.start_button, point)) return target::start_service;
  if (PtInRect(&layout.stop_button, point)) return target::stop_service;
  if (PtInRect(&layout.autostart_hit, point)) return target::autostart;
  if (PtInRect(&layout.logs_button, point)) return target::open_logs;
  return target::none;
}

// UI thread only.
host_snapshot g_view;
std::wstring g_port_text = L"28418";
bool g_port_dirty = false;
std::wstring g_long_text;
int g_focus = 0;  // 0 none, 1 port, 2 long-term
std::size_t g_caret = 0;
bool g_ui_remote = false;
bool g_ui_discovery = false;
bool g_ui_input = true;
bool g_ui_clipboard = true;
bool g_ui_multi = false;
bool g_ui_initialized = false;
bool g_autostart = false;
std::wstring g_message;
bool g_message_error = false;
ULONGLONG g_message_until = 0;
target g_hot = target::none;
bool g_tracking_mouse = false;

// ---------------------------------------------------------------------------
// Direct2D resources
// ---------------------------------------------------------------------------

struct theme_colors {
  D2D1_COLOR_F background;
  D2D1_COLOR_F panel;
  D2D1_COLOR_F text;
  D2D1_COLOR_F muted;
  D2D1_COLOR_F accent;
  D2D1_COLOR_F green;
  D2D1_COLOR_F amber;
  D2D1_COLOR_F red;
  D2D1_COLOR_F border;
  D2D1_COLOR_F field;
  D2D1_COLOR_F button;
  D2D1_COLOR_F button_hot;
  D2D1_COLOR_F button_disabled;
  D2D1_COLOR_F white;
};

const theme_colors& theme() {
  static const theme_colors colors = {
      D2D1::ColorF(0xF5F7FA), D2D1::ColorF(0xFFFFFF),
      D2D1::ColorF(0x1F2328), D2D1::ColorF(0x6B7280),
      D2D1::ColorF(0x1A73E8), D2D1::ColorF(0x1E9E5A),
      D2D1::ColorF(0xD97706), D2D1::ColorF(0xD93025),
      D2D1::ColorF(0xD8DEE6), D2D1::ColorF(0xFFFFFF),
      D2D1::ColorF(0xFFFFFF), D2D1::ColorF(0xEDF2F7),
      D2D1::ColorF(0xF1F3F5), D2D1::ColorF(0xFFFFFF)};
  return colors;
}

com_ptr<ID2D1Factory> g_d2d_factory;
com_ptr<IDWriteFactory> g_dwrite_factory;
com_ptr<ID2D1HwndRenderTarget> g_render_target;
com_ptr<ID2D1SolidColorBrush> g_brush_background;
com_ptr<ID2D1SolidColorBrush> g_brush_panel;
com_ptr<ID2D1SolidColorBrush> g_brush_text;
com_ptr<ID2D1SolidColorBrush> g_brush_muted;
com_ptr<ID2D1SolidColorBrush> g_brush_accent;
com_ptr<ID2D1SolidColorBrush> g_brush_green;
com_ptr<ID2D1SolidColorBrush> g_brush_amber;
com_ptr<ID2D1SolidColorBrush> g_brush_red;
com_ptr<ID2D1SolidColorBrush> g_brush_border;
com_ptr<ID2D1SolidColorBrush> g_brush_field;
com_ptr<ID2D1SolidColorBrush> g_brush_button;
com_ptr<ID2D1SolidColorBrush> g_brush_button_hot;
com_ptr<ID2D1SolidColorBrush> g_brush_button_disabled;
com_ptr<ID2D1SolidColorBrush> g_brush_white;
com_ptr<IDWriteTextFormat> g_format_title;
com_ptr<IDWriteTextFormat> g_format_label;
com_ptr<IDWriteTextFormat> g_format_value;
com_ptr<IDWriteTextFormat> g_format_code;
com_ptr<IDWriteTextFormat> g_format_small;
com_ptr<IDWriteTextFormat> g_format_small_right;
com_ptr<IDWriteTextFormat> g_format_button;
D2D1_SIZE_U g_render_size{};

bool create_text_format(const wchar_t* family, DWRITE_FONT_WEIGHT weight,
                        float size, DWRITE_TEXT_ALIGNMENT alignment,
                        com_ptr<IDWriteTextFormat>& format) {
  if (FAILED(g_dwrite_factory->CreateTextFormat(
          family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
          DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", format.put()))) {
    return false;
  }
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  format->SetTextAlignment(alignment);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  return true;
}

bool create_brush(const D2D1_COLOR_F& color,
                  com_ptr<ID2D1SolidColorBrush>& brush) {
  return SUCCEEDED(
      g_render_target->CreateSolidColorBrush(color, brush.put()));
}

bool create_graphics_resources() {
  // Microsoft YaHei ships with Windows 7; DirectWrite falls back to the
  // system default if a trimmed WM6 image removed it.
  const wchar_t* family = L"Microsoft YaHei";
  const bool formats =
      create_text_format(family, DWRITE_FONT_WEIGHT_BOLD, 13.5f,
                         DWRITE_TEXT_ALIGNMENT_LEADING, g_format_title) &&
      create_text_format(family, DWRITE_FONT_WEIGHT_NORMAL, 12.5f,
                         DWRITE_TEXT_ALIGNMENT_LEADING, g_format_label) &&
      create_text_format(family, DWRITE_FONT_WEIGHT_NORMAL, 13.0f,
                         DWRITE_TEXT_ALIGNMENT_LEADING, g_format_value) &&
      create_text_format(family, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f,
                         DWRITE_TEXT_ALIGNMENT_LEADING, g_format_code) &&
      create_text_format(family, DWRITE_FONT_WEIGHT_NORMAL, 11.0f,
                         DWRITE_TEXT_ALIGNMENT_LEADING, g_format_small) &&
      create_text_format(family, DWRITE_FONT_WEIGHT_NORMAL, 11.0f,
                         DWRITE_TEXT_ALIGNMENT_TRAILING,
                         g_format_small_right) &&
      create_text_format(family, DWRITE_FONT_WEIGHT_NORMAL, 12.0f,
                         DWRITE_TEXT_ALIGNMENT_CENTER, g_format_button);
  if (!formats) return false;
  g_format_button->SetParagraphAlignment(
      DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  const theme_colors& colors = theme();
  return create_brush(colors.background, g_brush_background) &&
         create_brush(colors.panel, g_brush_panel) &&
         create_brush(colors.text, g_brush_text) &&
         create_brush(colors.muted, g_brush_muted) &&
         create_brush(colors.accent, g_brush_accent) &&
         create_brush(colors.green, g_brush_green) &&
         create_brush(colors.amber, g_brush_amber) &&
         create_brush(colors.red, g_brush_red) &&
         create_brush(colors.border, g_brush_border) &&
         create_brush(colors.field, g_brush_field) &&
         create_brush(colors.button, g_brush_button) &&
         create_brush(colors.button_hot, g_brush_button_hot) &&
         create_brush(colors.button_disabled, g_brush_button_disabled) &&
         create_brush(colors.white, g_brush_white);
}

void discard_graphics() {
  g_render_target.reset();
  g_brush_background.reset();
  g_brush_panel.reset();
  g_brush_text.reset();
  g_brush_muted.reset();
  g_brush_accent.reset();
  g_brush_green.reset();
  g_brush_amber.reset();
  g_brush_red.reset();
  g_brush_border.reset();
  g_brush_field.reset();
  g_brush_button.reset();
  g_brush_button_hot.reset();
  g_brush_button_disabled.reset();
  g_brush_white.reset();
  g_format_title.reset();
  g_format_label.reset();
  g_format_value.reset();
  g_format_code.reset();
  g_format_small.reset();
  g_format_small_right.reset();
  g_format_button.reset();
  g_render_size = D2D1_SIZE_U{};
}

bool ensure_graphics() {
  RECT client{};
  GetClientRect(g_window, &client);
  const UINT32 width =
      client.right > client.left ? static_cast<UINT32>(client.right - client.left)
                                 : 1u;
  const UINT32 height =
      client.bottom > client.top ? static_cast<UINT32>(client.bottom - client.top)
                                 : 1u;
  if (!g_render_target) {
    const HRESULT created = g_d2d_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(g_window, D2D1::SizeU(width, height),
                                         D2D1_PRESENT_OPTIONS_NONE),
        g_render_target.put());
    if (FAILED(created)) return false;
    g_render_size = D2D1::SizeU(width, height);
    return create_graphics_resources();
  }
  if (g_render_size.width != width || g_render_size.height != height) {
    if (FAILED(g_render_target->Resize(D2D1::SizeU(width, height)))) {
      return false;
    }
    g_render_size = D2D1::SizeU(width, height);
  }
  return g_format_title && g_brush_text;
}

D2D1_RECT_F to_d2d(const RECT& rect) {
  return D2D1::RectF(static_cast<float>(rect.left),
                     static_cast<float>(rect.top),
                     static_cast<float>(rect.right),
                     static_cast<float>(rect.bottom));
}

void fill_round(const D2D1_RECT_F& rect, float radius, ID2D1Brush* brush) {
  if (brush == nullptr) return;
  g_render_target->FillRoundedRectangle(
      D2D1::RoundedRect(rect, radius, radius), brush);
}

void stroke_round(const D2D1_RECT_F& rect, float radius, ID2D1Brush* brush,
                  float width = 1.0f) {
  if (brush == nullptr) return;
  g_render_target->DrawRoundedRectangle(
      D2D1::RoundedRect(rect, radius, radius), brush, width);
}

void draw_text(const std::wstring& text, IDWriteTextFormat* format,
               const D2D1_RECT_F& rect, ID2D1Brush* brush) {
  if (text.empty() || format == nullptr || brush == nullptr) return;
  g_render_target->DrawText(
      text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
      D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void draw_button(const RECT& rect, const std::wstring& label, bool enabled,
                 bool hot) {
  const D2D1_RECT_F box = to_d2d(rect);
  ID2D1Brush* fill = enabled
                         ? (hot ? g_brush_button_hot.get()
                                : g_brush_button.get())
                         : g_brush_button_disabled.get();
  fill_round(box, 5.0f, fill);
  stroke_round(box, 5.0f, g_brush_border.get());
  draw_text(label, g_format_button.get(), box,
            enabled ? g_brush_text.get() : g_brush_muted.get());
}

void draw_toggle(const RECT& rect, bool on, bool enabled) {
  const D2D1_RECT_F box = to_d2d(rect);
  const float radius = (box.bottom - box.top) * 0.5f;
  ID2D1Brush* background =
      enabled ? (on ? g_brush_accent.get() : g_brush_border.get())
              : g_brush_button_disabled.get();
  fill_round(box, radius, background);
  const float knob_radius = radius - 4.0f;
  const float center_x = on ? box.right - radius : box.left + radius;
  const float center_y = (box.top + box.bottom) * 0.5f;
  g_render_target->FillEllipse(
      D2D1::Ellipse(D2D1::Point2F(center_x, center_y), knob_radius,
                    knob_radius),
      g_brush_white.get());
}

void draw_checkbox(const RECT& rect, bool checked, bool enabled) {
  const D2D1_RECT_F box = to_d2d(rect);
  fill_round(box, 3.0f, checked ? g_brush_accent.get()
                                : g_brush_field.get());
  stroke_round(box, 3.0f, checked ? g_brush_accent.get()
                                  : g_brush_border.get());
  if (!checked) return;
  ID2D1Brush* check = enabled ? g_brush_white.get() : g_brush_muted.get();
  const float mid_x = (box.left + box.right) * 0.5f;
  const float mid_y = (box.top + box.bottom) * 0.5f;
  g_render_target->DrawLine(D2D1::Point2F(box.left + 3.0f, mid_y),
                            D2D1::Point2F(mid_x, box.bottom - 3.0f), check,
                            1.6f);
  g_render_target->DrawLine(D2D1::Point2F(mid_x, box.bottom - 3.0f),
                            D2D1::Point2F(box.right - 3.0f, box.top + 3.0f),
                            check, 1.6f);
}

void draw_card(const D2D1_RECT_F& rect, const wchar_t* title) {
  fill_round(rect, 8.0f, g_brush_panel.get());
  stroke_round(rect, 8.0f, g_brush_border.get());
  draw_text(title, g_format_title.get(),
            D2D1::RectF(rect.left + 12.0f, rect.top + 4.0f,
                        rect.right - 12.0f, rect.top + 24.0f),
            g_brush_text.get());
}

void draw_edit(const RECT& rect, const std::wstring& text,
               const std::wstring& placeholder, bool focused,
               std::size_t caret) {
  const D2D1_RECT_F box = to_d2d(rect);
  fill_round(box, 5.0f, g_brush_field.get());
  stroke_round(box, 5.0f,
               focused ? g_brush_accent.get() : g_brush_border.get(),
               focused ? 1.5f : 1.0f);
  const bool empty = text.empty();
  draw_text(empty ? placeholder : text, g_format_value.get(),
            D2D1::RectF(box.left + 7.0f, box.top + 2.0f, box.right - 6.0f,
                        box.bottom - 2.0f),
            empty ? g_brush_muted.get() : g_brush_text.get());
  if (!focused) return;
  float x = box.left + 7.0f;
  if (!text.empty() && caret > 0) {
    com_ptr<IDWriteTextLayout> layout;
    const HRESULT created = g_dwrite_factory->CreateTextLayout(
        text.c_str(), static_cast<UINT32>(caret), g_format_value.get(),
        box.right - box.left, box.bottom - box.top, layout.put());
    if (SUCCEEDED(created)) {
      DWRITE_TEXT_METRICS metrics{};
      if (SUCCEEDED(layout->GetMetrics(&metrics))) x += metrics.width;
    }
  }
  x = (std::min)(x, box.right - 3.0f);
  g_render_target->DrawLine(D2D1::Point2F(x, box.top + 4.0f),
                            D2D1::Point2F(x, box.bottom - 4.0f),
                            g_brush_text.get(), 1.0f);
}

// ---------------------------------------------------------------------------
// Messages/actions
// ---------------------------------------------------------------------------

void set_message(const std::wstring& text, bool error = false) {
  g_message = text;
  g_message_error = error;
  g_message_until = GetTickCount64() + 8000;
  if (g_window != nullptr) InvalidateRect(g_window, nullptr, FALSE);
}

std::string truncated_utf8(const std::string& text) {
  return text.size() > 1024 ? text.substr(0, 1024) : text;
}

std::wstring api_error_text(const std::string& error) {
  return utf8_to_wide(truncated_utf8(error));
}

bool set_clipboard_text(const std::wstring& text) {
  if (!OpenClipboard(g_window)) return false;
  EmptyClipboard();
  const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory == nullptr) {
    CloseClipboard();
    return false;
  }
  void* destination = GlobalLock(memory);
  if (destination == nullptr) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }
  std::memcpy(destination, text.c_str(), bytes);
  GlobalUnlock(memory);
  if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }
  // The clipboard now owns the block.
  CloseClipboard();
  return true;
}

bool autostart_enabled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                    KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  wchar_t value[MAX_PATH * 2] = {};
  DWORD size = sizeof(value);
  DWORD type = 0;
  const LSTATUS queried = RegQueryValueExW(
      key, kRunValueName, nullptr, &type, reinterpret_cast<BYTE*>(value),
      &size);
  RegCloseKey(key);
  return queried == ERROR_SUCCESS &&
         (type == REG_SZ || type == REG_EXPAND_SZ) && size > sizeof(wchar_t);
}

bool set_autostart(bool enabled, std::string& error) {
  HKEY key = nullptr;
  const LSTATUS opened = RegCreateKeyExW(
      HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
      0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (opened != ERROR_SUCCESS) {
    error = "无法打开开机自启注册表项，错误码 " +
            std::to_string(opened);
    return false;
  }
  LSTATUS result = ERROR_SUCCESS;
  if (enabled) {
    const std::wstring command = L"\"" + module_path() + L"\"";
    result = RegSetValueExW(
        key, kRunValueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
  } else {
    result = RegDeleteValueW(key, kRunValueName);
    if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
  }
  RegCloseKey(key);
  if (result != ERROR_SUCCESS) {
    error = "无法写入开机自启，错误码 " + std::to_string(result);
    return false;
  }
  return true;
}

int configured_port() {
  if (g_view.state.valid && g_view.state.tcp_port > 0) {
    return g_view.state.tcp_port;
  }
  if (g_view.device.port > 0) return g_view.device.port;
  return kDefaultPort;
}

void save_settings() {
  if (!g_view.core_online) {
    set_message(L"本机核心未运行，设置未保存", true);
    return;
  }
  if (!g_ui_input) g_ui_clipboard = false;
  const std::string policy =
      molink::hostui::access_policy_from_switches(g_ui_input, g_ui_clipboard);
  const std::wstring path =
      L"/api/settings?" +
      widen_ascii(molink::hostui::settings_query(
          g_ui_remote, g_ui_discovery, true, policy));
  std::string error;
  if (post_api(path, error)) {
    set_message(L"设置已保存");
  } else {
    set_message(L"保存失败：" + api_error_text(error), true);
  }
  g_poll_now = true;
}

void commit_port() {
  if (!g_port_dirty) return;
  int port = 0;
  if (!molink::hostui::parse_listen_port(wide_to_utf8(g_port_text), port)) {
    set_message(L"端口必须是数字，范围 1-65535", true);
    const int fallback = configured_port();
    g_port_text = std::to_wstring(fallback);
    g_port_dirty = false;
    return;
  }
  if (!g_view.core_online) {
    set_message(L"本机核心未运行，端口未保存", true);
    return;
  }
  const int udp = g_view.state.udp_port > 0 ? g_view.state.udp_port
                                            : kDefaultUdpPort;
  const std::wstring path =
      L"/api/network?" +
      widen_ascii(molink::hostui::network_query(port, udp));
  std::string error;
  if (post_api(path, error)) {
    g_port_dirty = false;
    const int bound = g_view.state.bound_port > 0
                          ? g_view.state.bound_port
                          : g_view.device.port;
    if (bound > 0 && port != bound) {
      set_message(L"端口已保存，重启核心后生效");
    } else {
      set_message(L"端口已保存");
    }
  } else {
    set_message(L"保存端口失败：" + api_error_text(error), true);
  }
  g_poll_now = true;
}

void change_code_mode(bool multi) {
  if (multi == g_ui_multi) return;
  if (!g_view.core_online) {
    set_message(L"本机核心未运行，验证码模式未更改", true);
    return;
  }
  const std::wstring path =
      L"/api/auth/code?" +
      widen_ascii(molink::hostui::auth_code_query(
          multi ? "multi" : "single", 20, g_view.device.code));
  std::string error;
  if (post_api(path, error)) {
    g_ui_multi = multi;
    set_message(multi ? L"验证码模式已切换为多次"
                      : L"验证码模式已切换为单次");
  } else {
    set_message(L"切换失败：" + api_error_text(error), true);
  }
  g_poll_now = true;
}

void save_long_term() {
  if (g_long_text.empty()) {
    set_message(L"请输入新的长期密码（留空表示不修改）", true);
    return;
  }
  const std::string value = wide_to_utf8(g_long_text);
  std::string validation;
  if (!molink::hostui::valid_long_term_code(value, validation)) {
    set_message(utf8_to_wide(validation), true);
    return;
  }
  if (!g_view.core_online) {
    set_message(L"本机核心未运行，长期密码未保存", true);
    return;
  }
  const std::wstring path =
      L"/api/auth/long-term?" +
      widen_ascii(molink::hostui::long_term_query(value));
  std::string error;
  if (post_api(path, error)) {
    g_long_text.clear();
    g_focus = 0;
    set_message(L"长期密码已更新");
  } else {
    set_message(L"长期密码保存失败：" + api_error_text(error), true);
  }
  g_poll_now = true;
}

void copy_code() {
  if (g_view.device.code.empty()) {
    set_message(L"当前没有可用验证码", true);
    return;
  }
  if (set_clipboard_text(utf8_to_wide(g_view.device.code))) {
    set_message(L"验证码已复制");
  } else {
    set_message(L"复制验证码失败", true);
  }
}

void open_log_directory() {
  const std::wstring directory = log_directory();
  if (directory.empty() || !ensure_directory(directory)) {
    set_message(L"无法创建日志目录", true);
    return;
  }
  const HINSTANCE result = ShellExecuteW(g_window, L"open", directory.c_str(),
                                         nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    set_message(L"无法打开日志目录", true);
  }
}

bool service_installed_now() {
  const service_status status = molink::hostui::query_host_service();
  return status.installed;
}

void install_service() {
  set_message(L"正在安装服务，请在管理员授权窗口中确认...");
  InvalidateRect(g_window, nullptr, FALSE);
  UpdateWindow(g_window);
  std::string error;
  if (molink::hostui::install_host_service(error)) {
    set_message(L"服务已安装并启动");
  } else {
    set_message(L"安装服务失败：" + api_error_text(error), true);
  }
  g_poll_now = true;
}

/// Stop the service and whatever core is answering. The full product's
/// service deliberately keeps its core alive on a plain stop; the host-only
/// product's "停止" means "this machine must not be controllable any more".
void stop_service_and_core() {
  std::string error;
  bool ok = true;
  const service_status status = molink::hostui::query_host_service();
  if (status.installed) {
    std::string service_error;
    if (!molink::hostui::stop_host_service(service_error)) {
      ok = false;
      error = service_error;
    }
  }
  if (g_view.core_online && g_view.device.pid > 0) {
    std::string kill_error;
    if (!kill_process_by_pid(g_view.device.pid, kill_error)) {
      if (ok) error = kill_error;
      ok = false;
    }
  }
  g_stop_core_request = true;
  if (ok) {
    set_message(L"已停止");
  } else {
    set_message(L"停止失败：" + api_error_text(error), true);
  }
  g_poll_now = true;
}

void uninstall_service() {
  // Remove the service and make sure its core is not left reachable.
  const service_status status = molink::hostui::query_host_service();
  std::string stop_error;
  if (status.installed) molink::hostui::stop_host_service(stop_error);
  if (g_view.core_online && g_view.device.pid > 0) {
    std::string kill_error;
    kill_process_by_pid(g_view.device.pid, kill_error);
  }
  g_stop_core_request = true;

  set_message(L"正在卸载服务，请在管理员授权窗口中确认...");
  InvalidateRect(g_window, nullptr, FALSE);
  UpdateWindow(g_window);
  std::string error;
  if (molink::hostui::uninstall_host_service(error)) {
    set_message(L"服务已卸载");
  } else {
    set_message(L"卸载服务失败：" + api_error_text(error), true);
  }
  g_poll_now = true;
}

void start_service_or_core() {
  const service_status status = molink::hostui::query_host_service();
  if (status.installed) {
    set_message(L"正在启动服务...");
    InvalidateRect(g_window, nullptr, FALSE);
    UpdateWindow(g_window);
    std::string error;
    if (molink::hostui::start_host_service(error)) {
      set_message(L"服务已启动");
    } else {
      set_message(L"启动失败：" + api_error_text(error), true);
    }
  } else {
    g_start_core_request = true;
    set_message(L"正在启动核心...");
  }
  g_poll_now = true;
}

void toggle_autostart() {
  std::string error;
  const bool next = !g_autostart;
  if (!set_autostart(next, error)) {
    set_message(L"设置开机自启失败：" + api_error_text(error), true);
    return;
  }
  g_autostart = next;
  set_message(next ? L"已设置开机自启" : L"已取消开机自启");
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void draw_status_card() {
  draw_card(D2D1::RectF(10, 4, 510, 98), L"状态");

  const std::wstring device_id =
      g_view.device.valid ? utf8_to_wide(g_view.device.device_id) : L"--";
  const std::wstring code =
      g_view.device.valid ? utf8_to_wide(molink::hostui::code_summary(
                                g_view.device))
                          : L"--";
  const bool remote =
      molink::hostui::effective_remote_access(g_view.settings, g_view.state);
  const std::wstring address =
      g_view.device.valid
          ? utf8_to_wide(molink::hostui::join_addresses(
                g_view.device, remote, g_view.device.port))
          : L"--";

  draw_text(L"本机 ID", g_format_label.get(),
            D2D1::RectF(22, 30, 90, 50), g_brush_muted.get());
  draw_text(device_id, g_format_value.get(),
            D2D1::RectF(92, 29, 498, 50), g_brush_text.get());

  draw_text(L"验证码", g_format_label.get(),
            D2D1::RectF(22, 54, 90, 75), g_brush_muted.get());
  draw_text(code, g_format_code.get(), D2D1::RectF(92, 53, 410, 75),
            g_brush_text.get());
  draw_button(compute_layout().copy_button, L"复制",
              g_view.device.valid && !g_view.device.code.empty(),
              g_hot == target::copy_code);

  draw_text(L"可达地址", g_format_label.get(),
            D2D1::RectF(22, 78, 90, 98), g_brush_muted.get());
  draw_text(address, g_format_value.get(),
            D2D1::RectF(92, 77, 452, 98), g_brush_text.get());

  const float dot_x = 462.0f;
  const float dot_y = 88.0f;
  g_render_target->FillEllipse(
      D2D1::Ellipse(D2D1::Point2F(dot_x, dot_y), 5.0f, 5.0f),
      g_view.core_online ? g_brush_green.get() : g_brush_red.get());
  draw_text(utf8_to_wide(molink::hostui::online_text(g_view.core_online)),
            g_format_small.get(), D2D1::RectF(472, 78, 498, 98),
            g_view.core_online ? g_brush_green.get() : g_brush_muted.get());
}

void draw_network_card() {
  const layout_rects layout = compute_layout();
  draw_card(D2D1::RectF(10, 102, 510, 198), L"网络");

  draw_text(L"监听端口", g_format_label.get(),
            D2D1::RectF(22, 126, 90, 147), g_brush_muted.get());
  draw_edit(layout.port_edit, g_port_text, L"端口", g_focus == 1, g_caret);
  draw_text(L"重启核心后生效", g_format_small.get(),
            D2D1::RectF(162, 127, 300, 147), g_brush_muted.get());

  draw_text(L"允许远程访问", g_format_label.get(),
            D2D1::RectF(22, 151, 200, 172), g_brush_text.get());
  draw_toggle(layout.remote_toggle, g_ui_remote, true);

  draw_text(L"允许发现", g_format_label.get(),
            D2D1::RectF(22, 176, 200, 197), g_brush_text.get());
  draw_text(L"需先允许远程访问", g_format_small_right.get(),
            D2D1::RectF(280, 177, 430, 197), g_brush_muted.get());
  draw_toggle(layout.discovery_toggle, g_ui_discovery, true);
}

void draw_security_card() {
  const layout_rects layout = compute_layout();
  draw_card(D2D1::RectF(10, 202, 510, 326), L"安全");

  draw_text(L"验证码模式", g_format_label.get(),
            D2D1::RectF(22, 226, 200, 247), g_brush_text.get());
  draw_text(L"切换会刷新验证码", g_format_small_right.get(),
            D2D1::RectF(180, 227, 390, 247), g_brush_muted.get());
  const auto draw_segment = [](const RECT& rect, const wchar_t* label,
                               bool active) {
    const D2D1_RECT_F box = to_d2d(rect);
    fill_round(box, 4.0f,
               active ? g_brush_accent.get() : g_brush_field.get());
    stroke_round(box, 4.0f,
                 active ? g_brush_accent.get() : g_brush_border.get());
    draw_text(label, g_format_button.get(), box,
              active ? g_brush_white.get() : g_brush_text.get());
  };
  draw_segment(layout.mode_single, L"单次", !g_ui_multi);
  draw_segment(layout.mode_multi, L"多次", g_ui_multi);

  draw_text(L"长期密码", g_format_label.get(),
            D2D1::RectF(22, 252, 90, 273), g_brush_muted.get());
  const bool long_focused = g_focus == 2;
  // /api/settings carries the flag; /api/device's code object has the same
  // fact and keeps the field honest if the settings read failed this cycle.
  const bool long_set = g_view.settings.valid
                            ? g_view.settings.long_term_set
                            : g_view.device.long_term_set;
  const std::wstring long_shown =
      long_focused ? g_long_text : (long_set ? L"••••••••" : L"未设置");
  draw_edit(layout.long_edit, long_focused ? g_long_text : std::wstring(),
            long_focused ? L"输入新的长期密码" : long_shown, long_focused,
            g_caret);
  draw_button(layout.long_save, L"保存", !g_long_text.empty(),
              g_hot == target::long_save);
  draw_text(L"留空不修改", g_format_small.get(),
            D2D1::RectF(392, 253, 498, 273), g_brush_muted.get());

  draw_text(L"允许远程输入", g_format_label.get(),
            D2D1::RectF(22, 278, 200, 299), g_brush_text.get());
  draw_toggle(layout.input_toggle, g_ui_input, true);

  draw_text(L"允许远程剪贴板", g_format_label.get(),
            D2D1::RectF(22, 304, 220, 325),
            g_ui_input ? g_brush_text.get() : g_brush_muted.get());
  draw_toggle(layout.clipboard_toggle, g_ui_clipboard && g_ui_input,
              g_ui_input);
}

void draw_footer() {
  const layout_rects layout = compute_layout();
  const bool installed = g_view.service_installed;
  const bool core_online = g_view.core_online;

  draw_button(layout.install_button, L"安装服务", !installed,
              g_hot == target::install_service);
  draw_button(layout.uninstall_button, L"卸载服务", installed,
              g_hot == target::uninstall_service);
  draw_button(layout.start_button, L"启动",
              installed ? !g_view.service_running : !core_online,
              g_hot == target::start_service);
  draw_button(layout.stop_button, L"停止",
              installed ? g_view.service_running : core_online,
              g_hot == target::stop_service);

  draw_checkbox(layout.autostart_box, g_autostart, true);
  draw_text(L"开机自启", g_format_label.get(),
            D2D1::RectF(298, 336, 360, 362), g_brush_text.get());
  draw_button(layout.logs_button, L"打开日志目录", true,
              g_hot == target::open_logs);

  // Bottom line: the last action/error, or the live service+core summary.
  std::wstring status;
  ID2D1Brush* color = g_brush_muted.get();
  if (!g_message.empty() && GetTickCount64() < g_message_until) {
    status = g_message;
    color = g_message_error ? g_brush_red.get() : g_brush_text.get();
  } else if (!core_online && !g_view.error.empty()) {
    status = utf8_to_wide(g_view.error);
    color = g_brush_red.get();
  } else {
    const std::wstring service =
        g_view.service_state_text.empty()
            ? L"未知"
            : utf8_to_wide(g_view.service_state_text);
    status = L"服务：" + service + L" · 核心：" +
             utf8_to_wide(molink::hostui::online_text(core_online));
    color = core_online ? g_brush_green.get() : g_brush_muted.get();
  }
  draw_text(status, g_format_small.get(), D2D1::RectF(22, 374, 392, 394),
            color);
  draw_text(utf8_to_wide(molink::hostui::format_version(
                g_view.device.version)),
            g_format_small_right.get(),
            D2D1::RectF(398, 374, 510, 394), g_brush_muted.get());
}

void draw_ui() {
  g_render_target->Clear(theme().background);
  draw_status_card();
  draw_network_card();
  draw_security_card();
  draw_footer();
}

// ---------------------------------------------------------------------------
// Worker: find/adopt/spawn the core, poll the three endpoints
// ---------------------------------------------------------------------------

void start_core_if_needed() {
  core_conn connection;
  device_info device;
  if (find_core(connection, device)) {
    set_connection(connection);
    g_core_error.clear();
    return;
  }
  const service_status service = molink::hostui::query_host_service();
  if (service.installed) {
    g_core_error = service.running ? "服务已启动，正在等待核心..."
                                   : "服务已安装但未启动";
    return;
  }
  if (g_attach_only) {
    g_core_error = "指定的核心不可达，见环境变量 MOLINK_CORE_PORT";
    return;
  }
  if (spawned_core_alive()) {
    g_core_error = "核心正在启动...";
    return;
  }
  std::string error;
  if (spawn_core(error)) {
    g_core_error = "核心正在启动...";
  } else {
    g_core_error = error;
  }
}

void enforce_host_only(const core_conn& connection, const host_snapshot& snap) {
  if (!snap.settings.valid || snap.settings.host_only) return;
  const int identity = snap.device.pid > 0 ? snap.device.pid : connection.port;
  if (identity == g_host_only_enforced_pid) return;
  const ULONGLONG now = GetTickCount64();
  if (now < g_host_only_retry_at) return;
  core_conn authenticated = connection;
  authenticated.token = snap.device.token;
  authenticated.client_token = snap.device.client_token;
  const http_result result = http_request(
      authenticated, L"POST", L"/api/settings?host_only=1");
  if (result.ok) {
    g_host_only_enforced_pid = identity;
  } else {
    // Do not hammer a core that rejects the write; try again in 15 s.
    g_host_only_retry_at = now + 15000;
  }
}

host_snapshot collect_snapshot() {
  host_snapshot snapshot;
  const service_status service = molink::hostui::query_host_service();
  snapshot.service_installed = service.installed;
  snapshot.service_running = service.running;
  snapshot.service_state_text = molink::hostui::service_state_text(service.state);

  core_conn connection;
  device_info device;
  if (!find_core(connection, device)) {
    snapshot.core_online = false;
    snapshot.error = g_core_error;
    return snapshot;
  }
  set_connection(connection);
  snapshot.core_online = true;
  snapshot.device = device;

  http_result state_response =
      http_request(connection, L"GET", L"/api/state");
  if (state_response.ok) {
    std::string error;
    molink::hostui::parse_state(state_response.body, snapshot.state, error);
  }
  http_result settings_response =
      http_request(connection, L"GET", L"/api/settings");
  if (settings_response.ok) {
    std::string error;
    molink::hostui::parse_settings(settings_response.body, snapshot.settings,
                                   error);
  }
  enforce_host_only(connection, snapshot);
  g_core_error.clear();
  return snapshot;
}

void wait_briefly(int milliseconds) {
  for (int waited = 0; waited < milliseconds && g_running; waited += 100) {
    if (g_poll_now.exchange(false)) return;
    Sleep(100);
  }
}

void worker_main() {
  g_start_core_request = true;
  while (g_running) {
    if (g_stop_core_request.exchange(false)) {
      terminate_spawned_core();
      clear_connection();
      g_host_only_enforced_pid = 0;
    }
    if (g_start_core_request.exchange(false)) {
      start_core_if_needed();
    }
    host_snapshot snapshot = collect_snapshot();
    {
      std::lock_guard<std::mutex> lock(g_snapshot_mutex);
      g_snapshot = std::move(snapshot);
    }
    if (g_window != nullptr) {
      PostMessageW(g_window, kSnapshotMessage, 0, 0);
    }
    wait_briefly(1500);
  }
}

// ---------------------------------------------------------------------------
// UI snapshot application and input
// ---------------------------------------------------------------------------

void sync_ui_from_snapshot(const host_snapshot& snapshot) {
  g_view = snapshot;
  if (!snapshot.core_online) return;

  g_ui_remote =
      molink::hostui::effective_remote_access(snapshot.settings, snapshot.state);
  // A failed /api/settings read (e.g. a transient token race) must not flip
  // every switch back to its default; keep the last values until a good read.
  if (snapshot.settings.valid) {
    g_ui_discovery = snapshot.settings.discovery_enabled;
    const access_switches switches =
        molink::hostui::switches_from_access_policy(
            snapshot.settings.access_policy);
    g_ui_input = switches.input;
    g_ui_clipboard = switches.clipboard;
  }
  g_ui_multi = snapshot.device.code_mode == "multi";
  if (!g_port_dirty && g_focus != 1) {
    const int port = configured_port();
    if (port > 0) g_port_text = std::to_wstring(port);
  }
  g_ui_initialized = true;
}

bool clipboard_toggle_enabled() { return g_ui_input; }

void activate(target which) {
  switch (which) {
    case target::copy_code:
      copy_code();
      break;
    case target::port_edit:
      g_focus = 1;
      g_caret = g_port_text.size();
      break;
    case target::remote_toggle:
      g_ui_remote = !g_ui_remote;
      save_settings();
      break;
    case target::discovery_toggle:
      g_ui_discovery = !g_ui_discovery;
      save_settings();
      break;
    case target::mode_single:
      change_code_mode(false);
      break;
    case target::mode_multi:
      change_code_mode(true);
      break;
    case target::long_edit:
      if (g_focus != 2) g_long_text.clear();
      g_focus = 2;
      g_caret = g_long_text.size();
      break;
    case target::long_save:
      if (!g_long_text.empty()) save_long_term();
      break;
    case target::input_toggle:
      g_ui_input = !g_ui_input;
      if (!g_ui_input) g_ui_clipboard = false;
      save_settings();
      break;
    case target::clipboard_toggle:
      if (!clipboard_toggle_enabled()) {
        set_message(L"请先允许远程输入", true);
        break;
      }
      g_ui_clipboard = !g_ui_clipboard;
      save_settings();
      break;
    case target::install_service:
      if (!g_view.service_installed) install_service();
      break;
    case target::uninstall_service:
      if (g_view.service_installed) uninstall_service();
      break;
    case target::start_service:
      if (g_view.service_installed ? !g_view.service_running
                                   : !g_view.core_online) {
        start_service_or_core();
      }
      break;
    case target::stop_service:
      if (g_view.service_installed ? g_view.service_running
                                   : g_view.core_online) {
        stop_service_and_core();
      }
      break;
    case target::autostart:
      toggle_autostart();
      break;
    case target::open_logs:
      open_log_directory();
      break;
    case target::none:
      break;
  }
  InvalidateRect(g_window, nullptr, FALSE);
}

void handle_mouse_down(int x, int y) {
  const layout_rects layout = compute_layout();
  POINT point{x, y};
  const target which = hit_test(layout, point);
  // Leaving the port field commits it; leaving the long-term field discards
  // an unsaved draft. Clicking 保存 keeps the draft.
  if (g_focus == 1 && which != target::port_edit) commit_port();
  if (g_focus == 2 && which != target::long_edit &&
      which != target::long_save) {
    g_long_text.clear();
    g_focus = 0;
  }
  if (which != target::port_edit && which != target::long_edit &&
      which != target::long_save) {
    g_focus = 0;
  }
  activate(which);
}

void insert_character(wchar_t character) {
  std::wstring* text = nullptr;
  std::size_t limit = 0;
  if (g_focus == 1) {
    text = &g_port_text;
    limit = 5;
  } else if (g_focus == 2) {
    text = &g_long_text;
    limit = 128;
  } else {
    return;
  }
  if (text->size() >= limit) return;
  if (g_caret > text->size()) g_caret = text->size();
  text->insert(text->begin() + static_cast<std::ptrdiff_t>(g_caret), character);
  ++g_caret;
  if (g_focus == 1) g_port_dirty = true;
}

void erase_before_caret() {
  std::wstring* text = g_focus == 1 ? &g_port_text
                        : g_focus == 2 ? &g_long_text
                                       : nullptr;
  if (text == nullptr || g_caret == 0 || text->empty()) return;
  if (g_caret > text->size()) g_caret = text->size();
  text->erase(text->begin() + static_cast<std::ptrdiff_t>(g_caret - 1));
  --g_caret;
  if (g_focus == 1) g_port_dirty = true;
}

void erase_at_caret() {
  std::wstring* text = g_focus == 1 ? &g_port_text
                        : g_focus == 2 ? &g_long_text
                                       : nullptr;
  if (text == nullptr || g_caret >= text->size()) return;
  text->erase(text->begin() + static_cast<std::ptrdiff_t>(g_caret));
  if (g_focus == 1) g_port_dirty = true;
}

void handle_character(wchar_t character) {
  if (g_focus == 0) return;
  if (character == L'\b') {
    erase_before_caret();
  } else if (character == L'\r') {
    if (g_focus == 1) {
      commit_port();
      g_focus = 0;
    } else if (g_focus == 2) {
      save_long_term();
    }
  } else if (g_focus == 1) {
    if (character >= L'0' && character <= L'9') insert_character(character);
  } else if (g_focus == 2) {
    // Long-term codes are ASCII by the core's validation; rejecting whitespace
    // and control characters here keeps the field honest.
    if (character >= 33 && character <= 126) insert_character(character);
  }
  InvalidateRect(g_window, nullptr, FALSE);
}

bool handle_key(UINT key) {
  if (g_focus == 0) return false;
  std::wstring* text = g_focus == 1 ? &g_port_text : &g_long_text;
  switch (key) {
    case VK_BACK:
      // WM_CHAR carries the actual erase so a held key repeats correctly;
      // swallow the key-down here so DefWindowProc does not beep.
      return true;
    case VK_LEFT:
      if (g_caret > 0) --g_caret;
      break;
    case VK_RIGHT:
      if (g_caret < text->size()) ++g_caret;
      break;
    case VK_HOME:
      g_caret = 0;
      break;
    case VK_END:
      g_caret = text->size();
      break;
    case VK_DELETE:
      erase_at_caret();
      break;
    case VK_RETURN:
      if (g_focus == 1) {
        commit_port();
        g_focus = 0;
      } else {
        save_long_term();
      }
      break;
    case VK_ESCAPE:
      if (g_focus == 1) {
        g_port_dirty = false;
        g_port_text = std::to_wstring(configured_port());
      } else {
        g_long_text.clear();
      }
      g_focus = 0;
      g_caret = 0;
      break;
    case VK_TAB:
      g_focus = g_focus == 1 ? 2 : 1;
      g_caret = g_focus == 1 ? g_port_text.size() : g_long_text.size();
      break;
    default:
      return false;
  }
  InvalidateRect(g_window, nullptr, FALSE);
  return true;
}

// ---------------------------------------------------------------------------
// Tray icon and lifecycle
// ---------------------------------------------------------------------------

bool add_tray_icon() {
  if (g_tray_added) return true;
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = g_window;
  data.uID = kTrayId;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  data.uCallbackMessage = kTrayMessage;
  // The system application icon needs no external resource; the host-only
  // build is meant to be one unsigned exe plus a core.
  data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  lstrcpynW(data.szTip, kWindowTitle,
            static_cast<int>(std::size(data.szTip)));
  if (Shell_NotifyIconW(NIM_ADD, &data) == FALSE) return false;
  g_tray_added = true;
  return true;
}

void remove_tray_icon() {
  if (!g_tray_added) return;
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = g_window;
  data.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &data);
  g_tray_added = false;
}

void restore_window() {
  if (g_window == nullptr) return;
  ShowWindow(g_window, SW_SHOW);
  ShowWindow(g_window, SW_RESTORE);
  SetForegroundWindow(g_window);
  InvalidateRect(g_window, nullptr, FALSE);
}

void show_tray_menu() {
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) return;
  AppendMenuW(menu, MF_STRING, kMenuShow, L"显示窗口");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");
  POINT point{};
  GetCursorPos(&point);
  SetForegroundWindow(g_window);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y, 0,
                 g_window, nullptr);
  DestroyMenu(menu);
  PostMessageW(g_window, WM_NULL, 0, 0);
}

void finish_shutdown() {
  g_running = false;
  g_poll_now = true;
  if (g_worker.joinable()) g_worker.join();

  const bool installed = service_installed_now();
  // The service owns the core once it is installed, even if this shell
  // happened to spawn it first: the helper adopts a running core.
  if (installed) {
    close_spawned_handle();
  } else {
    terminate_spawned_core();
  }
  remove_tray_icon();
}

void request_exit(bool from_tray) {
  const service_status status = molink::hostui::query_host_service();
  if (from_tray && status.installed) {
    MessageBoxW(g_window,
                L"服务仍在后台运行。\n本机在界面关闭后仍可被远程控制。",
                L"moLink 被控端", MB_OK | MB_ICONINFORMATION);
  }
  finish_shutdown();
  if (g_window != nullptr) {
    DestroyWindow(g_window);
    g_window = nullptr;
  }
  PostQuitMessage(0);
}

/// Close button semantics. The decision is made from the SCM, never from a
/// cached flag: an install/uninstall done outside this UI applies immediately.
void handle_close() {
  const service_status status = molink::hostui::query_host_service();
  if (status.installed) {
    if (!add_tray_icon()) {
      // No tray means hiding the window would make the UI unreachable.
      request_exit(true);
      return;
    }
    ShowWindow(g_window, SW_HIDE);
    return;
  }
  if (status.state == service_state::unknown && !status.error.empty()) {
    MessageBoxW(g_window,
                L"无法查询被控端服务状态，将按未安装处理：\n关闭窗口会结束本进程启动的核心。",
                L"moLink 被控端", MB_OK | MB_ICONWARNING);
  }
  request_exit(false);
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      g_window = window;
      return 0;

    case kSnapshotMessage: {
      const host_snapshot snapshot = get_snapshot();
      sync_ui_from_snapshot(snapshot);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }

    case kTrayMessage:
      switch (LOWORD(lparam)) {
        case WM_LBUTTONDBLCLK:
          restore_window();
          break;
        case WM_RBUTTONUP:
          show_tray_menu();
          break;
        default:
          break;
      }
      return 0;

    case WM_COMMAND:
      if (LOWORD(wparam) == kMenuShow) {
        restore_window();
      } else if (LOWORD(wparam) == kMenuExit) {
        request_exit(true);
      }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(window, &paint);
      if (ensure_graphics()) {
        g_render_target->BeginDraw();
        g_render_target->SetTransform(D2D1::Matrix3x2F::Identity());
        draw_ui();
        const HRESULT result = g_render_target->EndDraw();
        if (result == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
          discard_graphics();
        }
      }
      EndPaint(window, &paint);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;

    case WM_SIZE:
      if (wparam == SIZE_MINIMIZED) {
        if (add_tray_icon()) ShowWindow(window, SW_HIDE);
      } else {
        discard_graphics();
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;

    case WM_GETMINMAXINFO: {
      // The layout is fixed; do not let the user drag a half-clipped window.
      MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lparam);
      RECT rect = rect_of(0, 0, kClientWidth, kClientHeight);
      AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                    WS_MINIMIZEBOX,
                         FALSE, 0);
      const LONG width = rect.right - rect.left;
      const LONG height = rect.bottom - rect.top;
      info->ptMinTrackSize.x = width;
      info->ptMinTrackSize.y = height;
      info->ptMaxTrackSize.x = width;
      info->ptMaxTrackSize.y = height;
      return 0;
    }

    case WM_TIMER:
      if (wparam == kUiTimer && !g_message.empty() &&
          GetTickCount64() >= g_message_until) {
        g_message.clear();
        g_message_error = false;
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;

    case WM_MOUSEMOVE: {
      if (!g_tracking_mouse) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = window;
        TrackMouseEvent(&track);
        g_tracking_mouse = true;
      }
      const layout_rects layout = compute_layout();
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const target hot = hit_test(layout, point);
      if (hot != g_hot) {
        g_hot = hot;
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      g_tracking_mouse = false;
      if (g_hot != target::none) {
        g_hot = target::none;
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;

    case WM_LBUTTONDOWN:
      SetFocus(window);
      handle_mouse_down(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      return 0;

    case WM_CHAR:
      handle_character(static_cast<wchar_t>(wparam));
      return 0;

    case WM_KEYDOWN:
      if (handle_key(static_cast<UINT>(wparam))) return 0;
      break;

    case WM_SETFOCUS:
      InvalidateRect(window, nullptr, FALSE);
      return 0;

    case WM_KILLFOCUS:
      if (g_focus == 1) commit_port();
      if (g_focus == 2) g_long_text.clear();
      g_focus = 0;
      g_caret = 0;
      InvalidateRect(window, nullptr, FALSE);
      return 0;

    case WM_CLOSE:
      handle_close();
      return 0;

    case WM_QUERYENDSESSION:
      return TRUE;

    case WM_ENDSESSION:
      if (wparam) {
        g_running = false;
        g_poll_now = true;
        PostQuitMessage(0);
      }
      return 0;

    case WM_DESTROY:
      KillTimer(window, kUiTimer);
      remove_tray_icon();
      g_window = nullptr;
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // Win7 has no SetProcessDpiAwarenessContext; the system-DPI call is enough
  // for a fixed-size window and keeps the shell usable on 100%-150% screens.
  SetProcessDPIAware();

  HANDLE single_instance = CreateMutexW(nullptr, TRUE, L"moLinkHostShell");
  if (single_instance != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
      ShowWindow(existing, SW_SHOW);
      ShowWindow(existing, SW_RESTORE);
      SetForegroundWindow(existing);
    }
    CloseHandle(single_instance);
    return 0;
  }

  // COM must be initialized before the factories; D2D 1.0 and DirectWrite are
  // the only COM services this shell needs.
  const HRESULT com = CoInitializeEx(
      nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  const bool com_initialized = SUCCEEDED(com);
  if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
    MessageBoxW(nullptr, L"COM 初始化失败，无法启动界面。", L"moLink 被控端",
                MB_OK | MB_ICONERROR);
    if (single_instance != nullptr) CloseHandle(single_instance);
    return 1;
  }

  HRESULT created = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                      g_d2d_factory.put());
  if (SUCCEEDED(created)) {
    created = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(g_dwrite_factory.put()));
  }
  if (FAILED(created)) {
    wchar_t message[160] = {};
    std::swprintf(message, std::size(message),
                  L"Direct2D/DirectWrite 初始化失败（0x%08lX）。\n"
                  L"Windows 7 需要安装 Platform Update。",
                  static_cast<unsigned long>(created));
    MessageBoxW(nullptr, message, L"moLink 被控端", MB_OK | MB_ICONERROR);
    g_dwrite_factory.reset();
    g_d2d_factory.reset();
    if (com_initialized) CoUninitialize();
    if (single_instance != nullptr) CloseHandle(single_instance);
    return 1;
  }

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  window_class.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) {
    MessageBoxW(nullptr, L"无法注册窗口类。", L"moLink 被控端",
                MB_OK | MB_ICONERROR);
    if (com_initialized) CoUninitialize();
    if (single_instance != nullptr) CloseHandle(single_instance);
    return 1;
  }

  // A plain overlapped window: no thick frame, no maximize box. The content
  // is a fixed 520x420 DIP layout.
  const DWORD style =
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT bounds = rect_of(0, 0, kClientWidth, kClientHeight);
  AdjustWindowRectEx(&bounds, style, FALSE, 0);
  g_window = CreateWindowExW(
      0, kWindowClass, kWindowTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
      bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr,
      instance, nullptr);
  if (g_window == nullptr) {
    MessageBoxW(nullptr, L"无法创建主窗口。", L"moLink 被控端",
                MB_OK | MB_ICONERROR);
    if (com_initialized) CoUninitialize();
    if (single_instance != nullptr) CloseHandle(single_instance);
    return 1;
  }

  ShowWindow(g_window, SW_SHOW);
  UpdateWindow(g_window);
  if (!ensure_graphics()) {
    MessageBoxW(g_window,
                L"Direct2D 渲染目标创建失败。\n"
                L"Windows 7 需要安装 Platform Update。",
                L"moLink 被控端", MB_OK | MB_ICONERROR);
    DestroyWindow(g_window);
    g_window = nullptr;
    g_dwrite_factory.reset();
    g_d2d_factory.reset();
    if (com_initialized) CoUninitialize();
    if (single_instance != nullptr) CloseHandle(single_instance);
    return 1;
  }

  g_autostart = autostart_enabled();
  int attach = 0;
  if (environment_port(attach)) {
    g_attach_only = true;
    g_attach_port = attach;
  }
  SetTimer(g_window, kUiTimer, 1000, nullptr);
  add_tray_icon();
  g_worker = std::thread(worker_main);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  // Covers WM_QUIT paths that did not go through request_exit (logoff).
  finish_shutdown();
  if (g_window != nullptr) {
    DestroyWindow(g_window);
    g_window = nullptr;
  }
  discard_graphics();
  g_dwrite_factory.reset();
  g_d2d_factory.reset();
  if (com_initialized) CoUninitialize();
  if (single_instance != nullptr) CloseHandle(single_instance);
  return static_cast<int>(message.wParam);
}
