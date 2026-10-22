// Experimental LocalSystem helper for Windows session control.
//
// It is intentionally minimal and not installed by default. A future core
// build will talk to the named pipe to list WTS sessions and switch between
// the physical console and an RDP session. Secure-desktop capture/input is
// NOT implemented here yet; that needs a helper injected into the active
// session and must be validated on a real Windows machine.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

namespace {

SERVICE_STATUS_HANDLE g_status = nullptr;
SERVICE_STATUS g_service_status{};
HANDLE g_stop_event = nullptr;
std::atomic<bool> g_running{true};
constexpr wchar_t kServiceName[] = L"moLinkSession";
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\molink-session";

std::string wide_to_utf8(const wchar_t* text) {
  if (text == nullptr) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 1) return std::string();
  std::string out(static_cast<std::size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::string json_escape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char ch : value) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += ch; break;
    }
  }
  return out;
}

std::string session_json() {
  PWTS_SESSION_INFOW sessions = nullptr;
  DWORD count = 0;
  if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions,
                             &count)) {
    return R"({"ok":false,"error":"WTSEnumerateSessions failed"})";
  }
  std::ostringstream out;
  out << R"({"ok":true,"sessions":[)";
  for (DWORD i = 0; i < count; ++i) {
    const WTS_SESSION_INFOW& session = sessions[i];
    LPWSTR station = nullptr;
    LPWSTR user = nullptr;
    DWORD bytes = 0;
    WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session.SessionId,
                                WTSWinStationName, &station, &bytes);
    WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session.SessionId,
                                WTSUserName, &user, &bytes);
    if (i != 0) out << ',';
    out << "{\"id\":" << session.SessionId
        << ",\"state\":" << static_cast<int>(session.State)
        << ",\"station\":\"" << json_escape(wide_to_utf8(station)) << '"'
        << ",\"user\":\"" << json_escape(wide_to_utf8(user)) << "\"}";
    if (station != nullptr) WTSFreeMemory(station);
    if (user != nullptr) WTSFreeMemory(user);
  }
  WTSFreeMemory(sessions);
  out << "]}";
  return out.str();
}

bool client_is_elevated(HANDLE pipe) {
  if (!ImpersonateNamedPipeClient(pipe)) return false;
  HANDLE token = nullptr;
  bool elevated = false;
  if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (GetTokenInformation(token, TokenElevation, &elevation,
                            sizeof(elevation), &size)) {
      elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
  }
  RevertToSelf();
  return elevated;
}

std::string handle_command(HANDLE pipe, const std::string& line) {
  std::istringstream input(line);
  std::string command;
  input >> command;
  if (command == "sessions") return session_json();
  if ((command == "kick" || command == "switch") &&
      !client_is_elevated(pipe)) {
    return R"({"ok":false,"error":"admin_required"})";
  }
  if (command == "kick") {
    DWORD session_id = 0;
    input >> session_id;
    if (session_id == 0 || session_id == WTSGetActiveConsoleSessionId()) {
      return R"({"ok":false,"error":"refusing to log off the console session"})";
    }
    if (!WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, session_id, FALSE)) {
      return R"({"ok":false,"error":"WTSLogoffSession failed"})";
    }
    return R"({"ok":true})";
  }
  if (command == "switch") {
    // switch <rdp-session>: move the RDP session back to the physical
    // console with tscon (the service runs as SYSTEM). Passwords are never
    // accepted or stored.
    DWORD rdp = 0;
    input >> rdp;
    if (rdp == 0) {
      return R"({"ok":false,"error":"switch needs an RDP session id"})";
    }
    wchar_t command[128] = {};
    swprintf(command, std::size(command), L"tscon.exe %lu /dest:console", rdp);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
      return R"({"ok":false,"error":"could not start tscon"})";
    }
    WaitForSingleObject(process.hProcess, 10000);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0 ? R"({"ok":true})"
                          : R"({"ok":false,"error":"tscon failed"})";
  }
  return R"({"ok":false,"error":"unknown command"})";
}

DWORD WINAPI control_handler(DWORD control, DWORD, void*, void*) {
  if (control == SERVICE_CONTROL_STOP ||
      control == SERVICE_CONTROL_SHUTDOWN) {
    g_running.store(false);
    if (g_stop_event != nullptr) SetEvent(g_stop_event);
    g_service_status.dwCurrentState = SERVICE_STOP_PENDING;
    SetServiceStatus(g_status, &g_service_status);
  }
  return NO_ERROR;
}

void report_running() {
  g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_service_status.dwCurrentState = SERVICE_RUNNING;
  g_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                        SERVICE_ACCEPT_SHUTDOWN;
  SetServiceStatus(g_status, &g_service_status);
}

void pipe_server() {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = FALSE;
  // SYSTEM and Administrators only. Do not expose session switching to an
  // unprivileged interactive process.
  ConvertStringSecurityDescriptorToSecurityDescriptorW(
      L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1,
      &attributes.lpSecurityDescriptor,
      nullptr);
  while (g_running.load()) {
    HANDLE pipe = CreateNamedPipeW(
        kPipeName, PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 64 * 1024,
        64 * 1024, 0, &attributes);
    if (pipe == INVALID_HANDLE_VALUE) {
      Sleep(500);
      continue;
    }
    const BOOL connected = ConnectNamedPipe(pipe, nullptr) != 0 ||
                           GetLastError() == ERROR_PIPE_CONNECTED;
    if (connected) {
      char buffer[4096] = {};
      DWORD read = 0;
      if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr) && read > 0) {
        buffer[read] = '\0';
        std::string reply = handle_command(pipe, buffer);
        DWORD written = 0;
        WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()),
                  &written, nullptr);
      }
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
  if (attributes.lpSecurityDescriptor != nullptr) {
    LocalFree(attributes.lpSecurityDescriptor);
  }
}

void WINAPI service_main(DWORD, wchar_t**) {
  g_status = RegisterServiceCtrlHandlerExW(kServiceName, control_handler, nullptr);
  if (g_status == nullptr) return;
  g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_service_status.dwCurrentState = SERVICE_START_PENDING;
  SetServiceStatus(g_status, &g_service_status);
  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  report_running();
  std::thread pipe_thread(pipe_server);
  WaitForSingleObject(g_stop_event, INFINITE);
  g_running.store(false);
  pipe_thread.join();
  if (g_stop_event != nullptr) CloseHandle(g_stop_event);
  g_service_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(g_status, &g_service_status);
}

bool install_service() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (manager == nullptr) return false;
  wchar_t path[MAX_PATH * 4] = {};
  GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  SC_HANDLE service = CreateServiceW(
      manager, kServiceName, L"moLink session helper",
      SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
      SERVICE_ERROR_NORMAL, path, nullptr, nullptr, nullptr, nullptr, nullptr);
  if (service == nullptr && GetLastError() == ERROR_SERVICE_EXISTS) {
    service = OpenServiceW(manager, kServiceName, SERVICE_ALL_ACCESS);
  }
  if (service == nullptr) {
    CloseServiceHandle(manager);
    return false;
  }
  ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                       SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr,
                       nullptr, nullptr, L"moLink session helper");
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return true;
}

bool uninstall_service() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) return false;
  SC_HANDLE service = OpenServiceW(manager, kServiceName, DELETE | SERVICE_STOP);
  if (service == nullptr) {
    CloseServiceHandle(manager);
    return false;
  }
  SERVICE_STATUS status{};
  ControlService(service, SERVICE_CONTROL_STOP, &status);
  const bool ok = DeleteService(service) != 0;
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc > 1 && wcscmp(argv[1], L"--install") == 0) {
    return install_service() ? 0 : 1;
  }
  if (argc > 1 && wcscmp(argv[1], L"--uninstall") == 0) {
    return uninstall_service() ? 0 : 1;
  }
  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<wchar_t*>(kServiceName), service_main},
      {nullptr, nullptr},
  };
  return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
