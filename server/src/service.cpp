#include "service.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cwchar>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "logging.h"
#include "runtime.h"
#include "version.h"

namespace molink {
namespace {

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
server_config g_service_config;
std::atomic<bool> g_service_stop{false};
int g_service_exit = 0;

std::wstring widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (needed <= 1) return std::wstring();
  std::wstring out(static_cast<std::size_t>(needed - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), needed);
  return out;
}

void set_status(DWORD state, DWORD win32_exit = NO_ERROR, DWORD wait_hint = 0) {
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = win32_exit;
  g_status.dwWaitHint = wait_hint;
  g_status.dwControlsAccepted =
      (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
  if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
    g_status.dwCheckPoint += 1;
  } else {
    g_status.dwCheckPoint = 0;
  }
  if (g_status_handle != nullptr) {
    SetServiceStatus(g_status_handle, &g_status);
  }
}

DWORD WINAPI service_control(DWORD control, DWORD, void*, void*) {
  switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      g_service_stop.store(true);
      set_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

void WINAPI service_main(DWORD, LPWSTR*) {
  g_status_handle = RegisterServiceCtrlHandlerExW(
      widen(g_service_config.service_name).c_str(), service_control, nullptr);
  if (g_status_handle == nullptr) return;

  set_status(SERVICE_START_PENDING, NO_ERROR, 10000);
  server_runtime runtime(g_service_config);
  std::string error;
  if (!runtime.initialize(error) || !runtime.start(error)) {
    log_error("service start failed: %s", error.c_str());
    g_service_exit = 1;
    set_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    return;
  }

  set_status(SERVICE_RUNNING);
  while (!g_service_stop.load() && !runtime.stopping().load()) {
    runtime.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  set_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
  runtime.request_stop();
  runtime.shutdown();
  set_status(SERVICE_STOPPED);
}

std::string executable_path() {
  std::wstring buffer(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return std::string();
  buffer.resize(length);
  const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), -1, nullptr, 0,
                                         nullptr, nullptr);
  if (needed <= 1) return std::string();
  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), -1, out.data(), needed, nullptr,
                      nullptr);
  return out;
}

std::string service_command_line(const server_config& config) {
  const std::string exe = executable_path();
  std::string command = "\"" + exe + "\" --service";
  if (!config.config_path.empty()) {
    command += " --config \"" + config.config_path + "\"";
  } else {
    command += " --data-dir \"" + config.data_dir + "\"";
  }
  return command;
}

}  // namespace

bool install_service(const server_config& config, std::string& error) {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (manager == nullptr) {
    error = "OpenSCManager failed: " + std::to_string(GetLastError());
    return false;
  }
  const std::wstring name = widen(config.service_name);
  const std::wstring display =
      widen(std::string(MOLINK_SERVER_PRODUCT) + " (" + config.service_name + ")");
  const std::wstring command = widen(service_command_line(config));

  SC_HANDLE service = CreateServiceW(
      manager, name.c_str(), display.c_str(), SERVICE_ALL_ACCESS,
      SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
      command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
  if (service == nullptr && GetLastError() == ERROR_SERVICE_EXISTS) {
    service = OpenServiceW(manager, name.c_str(), SERVICE_ALL_ACCESS);
    if (service != nullptr) {
      ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                           SERVICE_NO_CHANGE, command.c_str(), nullptr, nullptr,
                           nullptr, nullptr, nullptr, display.c_str());
    }
  }
  if (service == nullptr) {
    error = "CreateService failed: " + std::to_string(GetLastError());
    CloseServiceHandle(manager);
    return false;
  }

  std::wstring description =
      widen("moLink device directory, admin console and API");
  SERVICE_DESCRIPTIONW description_info{};
  description_info.lpDescription = description.data();
  ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description_info);

  SERVICE_DELAYED_AUTO_START_INFO delayed{};
  delayed.fDelayedAutostart = TRUE;
  ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);

  SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000},
                          {SC_ACTION_RESTART, 30000},
                          {SC_ACTION_RESTART, 60000}};
  SERVICE_FAILURE_ACTIONSW failure{};
  failure.dwResetPeriod = 86400;
  failure.cActions = 3;
  failure.lpsaActions = actions;
  ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

  if (!StartServiceW(service, 0, nullptr)) {
    const DWORD code = GetLastError();
    if (code != ERROR_SERVICE_ALREADY_RUNNING) {
      error = "StartService failed: " + std::to_string(code);
      CloseServiceHandle(service);
      CloseServiceHandle(manager);
      return false;
    }
  }
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return true;
}

bool uninstall_service(const server_config& config, std::string& error) {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) {
    error = "OpenSCManager failed: " + std::to_string(GetLastError());
    return false;
  }
  const std::wstring name = widen(config.service_name);
  SC_HANDLE service = OpenServiceW(
      manager, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
  if (service == nullptr) {
    const DWORD code = GetLastError();
    CloseServiceHandle(manager);
    if (code == ERROR_SERVICE_DOES_NOT_EXIST) return true;
    error = "OpenService failed: " + std::to_string(code);
    return false;
  }
  SERVICE_STATUS status{};
  ControlService(service, SERVICE_CONTROL_STOP, &status);
  for (int i = 0; i < 60; ++i) {
    if (!QueryServiceStatus(service, &status)) break;
    if (status.dwCurrentState == SERVICE_STOPPED) break;
    Sleep(500);
  }
  if (!DeleteService(service)) {
    error = "DeleteService failed: " + std::to_string(GetLastError());
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return false;
  }
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return true;
}

int run_service(server_config config) {
  g_service_config = std::move(config);
  g_service_stop.store(false);
  g_service_exit = 0;
  const std::wstring name = widen(g_service_config.service_name);
  wchar_t mutable_name[256] = {};
  wcsncpy(mutable_name, name.c_str(), 255);
  SERVICE_TABLE_ENTRYW table[2] = {
      {mutable_name, service_main},
      {nullptr, nullptr},
  };
  if (!StartServiceCtrlDispatcherW(table)) {
    const DWORD code = GetLastError();
    if (code == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
      std::fprintf(stderr,
                   "molink-server: --service must be launched by the Windows "
                   "service manager\n");
    } else {
      std::fprintf(stderr,
                   "molink-server: StartServiceCtrlDispatcher failed: %lu\n",
                   static_cast<unsigned long>(code));
    }
    return 2;
  }
  return g_service_exit;
}

}  // namespace molink

#else  // !_WIN32

namespace molink {

bool install_service(const server_config&, std::string& error) {
  error = "service installation is only supported on Windows";
  return false;
}

bool uninstall_service(const server_config&, std::string& error) {
  error = "service removal is only supported on Windows";
  return false;
}

int run_service(server_config) { return 2; }

}  // namespace molink

#endif
