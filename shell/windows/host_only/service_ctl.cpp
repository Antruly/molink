#include "service_ctl.h"

#include <iterator>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <vector>
#endif

namespace molink {
namespace hostui {
namespace {

#if defined(_WIN32)
constexpr wchar_t kServiceName[] = L"moLinkHost";
constexpr wchar_t kHelperName[] = L"molink-service.exe";

std::wstring module_directory() {
  wchar_t path[MAX_PATH * 4] = {};
  const DWORD length = GetModuleFileNameW(
      nullptr, path, static_cast<DWORD>(std::size(path)));
  if (length == 0 || length >= std::size(path)) return std::wstring();
  std::wstring full(path, length);
  const std::size_t slash = full.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring()
                                     : full.substr(0, slash);
}

bool file_exists(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring env_value(const wchar_t* name) {
  wchar_t value[MAX_PATH * 4] = {};
  const DWORD length = GetEnvironmentVariableW(
      name, value, static_cast<DWORD>(std::size(value)));
  if (length == 0 || length >= std::size(value)) return std::wstring();
  return std::wstring(value, length);
}

/// molink-service.exe next to this shell, then the copy the installer keeps
/// under %ProgramData%\moLink\service.
std::wstring find_service_helper() {
  const std::wstring override_path = env_value(L"MOLINK_SERVICE_EXE");
  if (file_exists(override_path)) return override_path;

  const std::wstring directory = module_directory();
  if (!directory.empty()) {
    const std::wstring candidate = directory + L"\\" + kHelperName;
    if (file_exists(candidate)) return candidate;
  }
  const std::wstring program_data = env_value(L"ProgramData");
  if (!program_data.empty()) {
    const std::wstring candidate =
        program_data + L"\\moLink\\service\\" + kHelperName;
    if (file_exists(candidate)) return candidate;
  }
  return std::wstring();
}

std::string error_code_text(const char* prefix, DWORD code) {
  return std::string(prefix) + "，错误码 " + std::to_string(code);
}

/// Run the helper with `runas` so install/uninstall can create or delete a
/// service. The app itself stays unelevated.
bool run_helper(const std::wstring& arguments, std::string& error) {
  const std::wstring helper = find_service_helper();
  if (helper.empty()) {
    error = "未找到服务安装程序，可设置环境变量 MOLINK_SERVICE_EXE";
    return false;
  }
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = helper.c_str();
  info.lpParameters = arguments.c_str();
  info.nShow = SW_HIDE;
  if (!ShellExecuteExW(&info)) {
    const DWORD code = GetLastError();
    if (code == ERROR_CANCELLED) {
      error = "已取消管理员授权";
    } else {
      error = error_code_text("无法启动服务安装程序", code);
    }
    return false;
  }
  if (info.hProcess == nullptr) {
    error = "服务安装程序没有返回进程句柄";
    return false;
  }
  const DWORD wait = WaitForSingleObject(info.hProcess, 120000);
  DWORD exit_code = 1;
  GetExitCodeProcess(info.hProcess, &exit_code);
  CloseHandle(info.hProcess);
  if (wait != WAIT_OBJECT_0) {
    error = "服务安装程序超时";
    return false;
  }
  if (exit_code != 0) {
    error = "服务安装程序返回错误码 " + std::to_string(exit_code);
    return false;
  }
  return true;
}

std::string service_query_error(DWORD code) {
  return error_code_text("查询服务失败", code);
}
#endif  // _WIN32

}  // namespace

const char* service_state_text(service_state state) {
  switch (state) {
    case service_state::not_installed: return "未安装";
    case service_state::stopped: return "已停止";
    case service_state::start_pending: return "正在启动";
    case service_state::stop_pending: return "正在停止";
    case service_state::running: return "运行中";
    case service_state::paused: return "已暂停";
    case service_state::other: return "其他状态";
    case service_state::unknown: break;
  }
  return "未知";
}

service_status query_host_service() {
  service_status status;
#if defined(_WIN32)
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) {
    status.error = error_code_text("无法连接服务管理器", GetLastError());
    return status;
  }
  SC_HANDLE service =
      OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
  if (service == nullptr) {
    const DWORD code = GetLastError();
    if (code == ERROR_SERVICE_DOES_NOT_EXIST) {
      status.state = service_state::not_installed;
    } else {
      status.error = service_query_error(code);
    }
    CloseServiceHandle(manager);
    return status;
  }
  SERVICE_STATUS native{};
  if (!QueryServiceStatus(service, &native)) {
    status.error = service_query_error(GetLastError());
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return status;
  }
  status.installed = true;
  status.running = native.dwCurrentState == SERVICE_RUNNING;
  switch (native.dwCurrentState) {
    case SERVICE_STOPPED: status.state = service_state::stopped; break;
    case SERVICE_START_PENDING:
      status.state = service_state::start_pending;
      break;
    case SERVICE_STOP_PENDING: status.state = service_state::stop_pending; break;
    case SERVICE_RUNNING: status.state = service_state::running; break;
    case SERVICE_PAUSED: status.state = service_state::paused; break;
    default: status.state = service_state::other; break;
  }
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
#else
  status.state = service_state::not_installed;
  status.error = "此功能仅限 Windows";
#endif
  return status;
}

bool start_host_service(std::string& error) {
#if defined(_WIN32)
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) {
    error = error_code_text("无法连接服务管理器", GetLastError());
    return false;
  }
  SC_HANDLE service = OpenServiceW(
      manager, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS);
  if (service == nullptr) {
    const DWORD code = GetLastError();
    error = code == ERROR_SERVICE_DOES_NOT_EXIST
                ? "服务未安装"
                : error_code_text("打开服务失败", code);
    CloseServiceHandle(manager);
    return false;
  }
  bool started = false;
  DWORD last_error = 0;
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (StartServiceW(service, 0, nullptr)) {
      started = true;
      break;
    }
    last_error = GetLastError();
    if (last_error == ERROR_SERVICE_ALREADY_RUNNING) {
      started = true;
      break;
    }
    // Only a start that timed out is worth another attempt; a missing binary
    // or denied access will not fix itself.
    if (last_error != ERROR_SERVICE_REQUEST_TIMEOUT) break;
    Sleep(300);
  }
  if (!started) error = error_code_text("启动服务失败", last_error);
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return started;
#else
  error = "此功能仅限 Windows";
  return false;
#endif
}

bool stop_host_service(std::string& error) {
#if defined(_WIN32)
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) {
    error = error_code_text("无法连接服务管理器", GetLastError());
    return false;
  }
  SC_HANDLE service = OpenServiceW(
      manager, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
  if (service == nullptr) {
    const DWORD code = GetLastError();
    error = code == ERROR_SERVICE_DOES_NOT_EXIST
                ? "服务未安装"
                : error_code_text("打开服务失败", code);
    CloseServiceHandle(manager);
    return false;
  }
  SERVICE_STATUS status{};
  if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
    const DWORD code = GetLastError();
    if (code != ERROR_SERVICE_NOT_ACTIVE) {
      error = error_code_text("停止服务失败", code);
      CloseServiceHandle(service);
      CloseServiceHandle(manager);
      return false;
    }
  }
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return true;
#else
  error = "此功能仅限 Windows";
  return false;
#endif
}

bool install_host_service(std::string& error) {
#if defined(_WIN32)
  if (!run_helper(L"--install --name moLinkHost", error)) return false;
  const service_status status = query_host_service();
  if (status.installed) return true;
  error = "安装程序已运行，但没有找到预期服务 moLinkHost";
  if (!status.error.empty()) error += "：" + status.error;
  return false;
#else
  error = "此功能仅限 Windows";
  return false;
#endif
}

bool uninstall_host_service(std::string& error) {
#if defined(_WIN32)
  const service_status before = query_host_service();
  if (!before.installed) return true;
  if (!run_helper(L"--uninstall --name moLinkHost", error)) return false;
  const service_status after = query_host_service();
  if (!after.installed) return true;
  error = "卸载程序已运行，但预期服务仍然存在 moLinkHost";
  if (!after.error.empty()) error += "：" + after.error;
  return false;
#else
  error = "此功能仅限 Windows";
  return false;
#endif
}

}  // namespace hostui
}  // namespace molink
