#include "service_control.h"

#include <cstdint>
#include <iterator>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
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
  const BOOL ok = StartServiceW(service, 0, nullptr) ||
                  GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
  if (!ok) err = "StartService failed";
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return ok != 0;
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
#else
  (void)err;
  return false;
#endif
}

bool install_session_service(std::string& err) {
#if defined(_WIN32)
  return run_elevated(L"runas", "--install", err);
#else
  (void)err;
  return false;
#endif
}

bool uninstall_session_service(std::string& err) {
#if defined(_WIN32)
  return run_elevated(L"runas", "--uninstall", err);
#else
  (void)err;
  return false;
#endif
}

}  // namespace molink
