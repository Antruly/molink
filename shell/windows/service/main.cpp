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
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <sstream>
#include <fstream>
#include <string>
#include <thread>

namespace {

SERVICE_STATUS_HANDLE g_status = nullptr;
SERVICE_STATUS g_service_status{};
HANDLE g_stop_event = nullptr;
/// Signals molink-helper.exe to stop its supervised core and exit, so a
/// session switch never leaves the old core behind. Global: the service runs
/// in session 0 and the helper in the console session.
HANDLE g_helper_stop_event = nullptr;
/// "Exit but leave the core running": used when the service itself is being
/// stopped (the app's Stop button), as opposed to a console session switch.
HANDLE g_helper_detach_event = nullptr;
PROCESS_INFORMATION g_helper{};
/// Serialises helper start/stop: the pipe thread's secure-start/secure-stop
/// can race the main loop's session switch.
std::mutex g_helper_mutex;
/// SERVICE_CONTROL_STOP is disabled while a session switch is in progress.
bool g_controls_switching = false;
/// Console session the helper was started in; the console moves when the user
/// picks "switch user" or logs back in.
DWORD g_helper_session = 0xFFFFFFFF;
std::atomic<bool> g_running{true};
constexpr wchar_t kServiceName[] = L"moLinkSession";
constexpr wchar_t kHelperStopEventName[] = L"Global\\moLinkHelperStop";
constexpr wchar_t kHelperDetachEventName[] = L"Global\\moLinkHelperDetach";
constexpr wchar_t kLogPath[] = L"C:\\ProgramData\\moLink\\service\\service.log";

void log_line(const std::string& message) {
  std::ofstream out(kLogPath, std::ios::app);
  if (out) out << message << "\n";
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

bool launch_helper(std::string& err) {
  std::lock_guard<std::mutex> lock(g_helper_mutex);
  if (g_helper.hProcess != nullptr) return true;
  DWORD session = WTSGetActiveConsoleSessionId();
  if (session == 0xFFFFFFFF) {
    err = "no active console session";
    return false;
  }
  // A relaunch must not inherit a stop request left over from the previous
  // helper (or from service shutdown).
  if (g_helper_stop_event != nullptr) ResetEvent(g_helper_stop_event);
  if (g_helper_detach_event != nullptr) ResetEvent(g_helper_detach_event);
  // Use the service's own SYSTEM token. WTSQueryUserToken fails at the login
  // screen because no user is logged on yet.
  HANDLE process_token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY,
                        &process_token)) {
    err = "OpenProcessToken failed";
    return false;
  }
  HANDLE token = nullptr;
  const BOOL duplicated = DuplicateTokenEx(
      process_token, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
      TokenPrimary, &token);
  CloseHandle(process_token);
  if (!duplicated) {
    err = "DuplicateTokenEx failed";
    return false;
  }
  if (!SetTokenInformation(token, TokenSessionId, &session,
                           sizeof(session))) {
    log_line("SetTokenInformation failed: " +
             std::to_string(GetLastError()));
    CloseHandle(token);
    return false;
  }
  wchar_t path[MAX_PATH * 4] = {};
  GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  std::wstring helper(path);
  const std::size_t slash = helper.find_last_of(L"\\/");
  helper = (slash == std::wstring::npos ? std::wstring()
                                        : helper.substr(0, slash + 1)) +
           L"molink-helper.exe";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  // Start on the interactive desktop; the helper switches each pipe thread to
  // whichever desktop currently receives input.
  const wchar_t* desktops[] = {L"winsta0\\default",
                               L"winsta0\\Winlogon"};
  BOOL created = FALSE;
  for (const wchar_t* desktop : desktops) {
    startup.lpDesktop = const_cast<wchar_t*>(desktop);
    created = CreateProcessAsUserW(
        token, helper.c_str(), nullptr, nullptr, nullptr, FALSE,
        CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT |
            CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &g_helper);
    if (created) {
      log_line(std::string("helper launched on ") +
               std::string(desktop, desktop + wcslen(desktop)) +
               " pid=" + std::to_string(g_helper.dwProcessId) +
               " session=" + std::to_string(session));
      break;
    }
    log_line(std::string("CreateProcessAsUser failed on ") +
             std::string(desktop, desktop + wcslen(desktop)) +
             " error=" + std::to_string(GetLastError()));
  }
  CloseHandle(token);
  if (!created) {
    err = "CreateProcessAsUser failed";
    return false;
  }
  g_helper_session = session;
  return true;
}

/// Stops the helper. `stop_core` is true for a console session switch, where
/// the old core must leave before the new session's helper starts; false for a
/// service stop, where only the secure-desktop helper goes away and the core
/// (which asked for the stop through the app) keeps running.
void stop_helper(bool stop_core) {
  std::lock_guard<std::mutex> lock(g_helper_mutex);
  if (g_helper.hProcess == nullptr) return;
  HANDLE event = stop_core ? g_helper_stop_event : g_helper_detach_event;
  if (event != nullptr) SetEvent(event);
  const DWORD wait = WaitForSingleObject(g_helper.hProcess, 8000);
  if (wait != WAIT_OBJECT_0) {
    log_line("helper did not stop in time, terminating pid=" +
             std::to_string(g_helper.dwProcessId));
    TerminateProcess(g_helper.hProcess, 0);
    WaitForSingleObject(g_helper.hProcess, 3000);
  }
  if (g_helper_stop_event != nullptr) ResetEvent(g_helper_stop_event);
  if (g_helper_detach_event != nullptr) ResetEvent(g_helper_detach_event);
  CloseHandle(g_helper.hThread);
  CloseHandle(g_helper.hProcess);
  g_helper = PROCESS_INFORMATION{};
  // Keep g_helper_session: it is the last session the helper ran in, so the
  // loop can still tell a session move from a same-session helper crash.
}

struct helper_state {
  bool running = false;
  DWORD session = 0xFFFFFFFF;
  DWORD pid = 0;
};

helper_state query_helper() {
  std::lock_guard<std::mutex> lock(g_helper_mutex);
  helper_state state;
  // The last session the helper ran in is meaningful even after it exited:
  // it tells the loop whether the console actually moved.
  state.session = g_helper_session;
  if (g_helper.hProcess == nullptr) return state;
  state.pid = g_helper.dwProcessId;
  state.running = WaitForSingleObject(g_helper.hProcess, 0) != WAIT_OBJECT_0;
  return state;
}

DWORD helper_pid() {
  std::lock_guard<std::mutex> lock(g_helper_mutex);
  return g_helper.dwProcessId;
}

std::wstring core_pid_path() {
  std::wstring path(kLogPath);
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return L"core.pid";
  return path.substr(0, slash + 1) + L"core.pid";
}

unsigned long long process_creation_ticks(HANDLE process) {
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

bool pid_exists(DWORD pid) {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    // Cannot tell; keep the record for the next attempt.
    return true;
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == pid) {
        found = true;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return found;
}

bool image_is_molink_core(HANDLE process) {
  wchar_t path[32768] = {};
  DWORD size = static_cast<DWORD>(std::size(path));
  if (!QueryFullProcessImageNameW(process, 0, path, &size)) return false;
  std::wstring name(path, size);
  const std::size_t slash = name.find_last_of(L"\\/");
  if (slash != std::wstring::npos) name = name.substr(slash + 1);
  return _wcsicmp(name.c_str(), L"molink-core.exe") == 0;
}

/// A helper that was terminated before it could stop its core leaves
/// core.pid behind. Kill that core so the new session does not inherit a
/// listener bound to the old console's desktop. The recorded creation time
/// guards against Windows having reused the pid for an unrelated process.
void terminate_stale_core() {
  const std::wstring path = core_pid_path();
  std::ifstream in(path.c_str());
  DWORD pid = 0;
  unsigned long long recorded_creation = 0;
  if (!(in >> pid) || pid == 0) return;
  in >> recorded_creation;  // optional; older files contain only the pid
  in.close();
  HANDLE process =
      OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION |
                      SYNCHRONIZE,
                  FALSE, pid);
  if (process == nullptr) {
    if (!pid_exists(pid)) DeleteFileW(path.c_str());
    return;
  }
  if (!image_is_molink_core(process)) {
    // The pid was reused by an unrelated process; the core it named is gone.
    CloseHandle(process);
    DeleteFileW(path.c_str());
    return;
  }
  const unsigned long long actual_creation = process_creation_ticks(process);
  if (recorded_creation != 0 && actual_creation != 0 &&
      recorded_creation != actual_creation) {
    CloseHandle(process);
    DeleteFileW(path.c_str());
    return;
  }
  if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
    if (TerminateProcess(process, 1)) {
      WaitForSingleObject(process, 3000);
      log_line("terminated stale core pid=" + std::to_string(pid));
    } else {
      log_line("could not terminate stale core pid=" + std::to_string(pid) +
               " error=" + std::to_string(GetLastError()));
    }
  }
  const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
  CloseHandle(process);
  // Keep the record when the core refused to die so the next switch retries.
  if (exited) DeleteFileW(path.c_str());
}

std::string handle_command(HANDLE pipe, const std::string& line) {
  std::istringstream input(line);
  std::string command;
  input >> command;
  if (command == "sessions") return session_json();
  if (command == "sas") {
    // SendInput cannot generate Ctrl+Alt+Del; only a service can, and only
    // when the SoftwareSASGeneration policy allows it.
    HMODULE sas = LoadLibraryW(L"sas.dll");
    if (sas == nullptr) {
      return R"({"ok":false,"error":"sas_dll_unavailable"})";
    }
    typedef BOOL(WINAPI * send_sas_fn)(BOOL);
    send_sas_fn send = reinterpret_cast<send_sas_fn>(
        GetProcAddress(sas, "SendSAS"));
    const BOOL sent = send != nullptr && send(FALSE);
    const DWORD code = sent ? 0 : GetLastError();
    FreeLibrary(sas);
    if (!sent) {
      return R"({"ok":false,"error":"send_sas_failed","code":)" +
             std::to_string(code) + "}";
    }
    return R"({"ok":true})";
  }
  if ((command == "kick" || command == "switch" ||
       command == "secure-start" || command == "secure-stop") &&
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
  if (command == "secure-start") {
    std::string err;
    if (!launch_helper(err)) {
      return R"({"ok":false,"error":"helper_launch_failed"})";
    }
    char reply[96] = {};
    std::snprintf(reply, sizeof(reply), R"({"ok":true,"pid":%lu})",
                  static_cast<unsigned long>(helper_pid()));
    return reply;
  }
  if (command == "secure-stop") {
    // Stopping the secure-desktop helper must not take the app's core down.
    stop_helper(false);
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

/// While the helper is being moved to another console session, the old core
/// runs its normal shutdown path, which ends with stop_session_service().
/// Rejecting SERVICE_CONTROL_STOP for that short window keeps a session switch
/// from stopping the service that is about to launch the new helper. System
/// shutdown is still accepted.
void report_switching_helper() {
  g_controls_switching = true;
  g_service_status.dwControlsAccepted = SERVICE_ACCEPT_SHUTDOWN;
  SetServiceStatus(g_status, &g_service_status);
}

void report_helper_switched() {
  g_controls_switching = false;
  g_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                        SERVICE_ACCEPT_SHUTDOWN;
  SetServiceStatus(g_status, &g_service_status);
}

void poke_pipe(const wchar_t* name) {
  for (int attempt = 0; attempt < 40; ++attempt) {
    HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(pipe, "quit", 4, &written, nullptr);
      CloseHandle(pipe);
      return;
    }
    Sleep(50);
  }
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
        // A disconnect discards messages the client has not read yet.
        FlushFileBuffers(pipe);
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
  log_line("service started");
  enable_privilege(L"SeTcbPrivilege");
  enable_privilege(L"SeAssignPrimaryTokenPrivilege");
  enable_privilege(L"SeIncreaseQuotaPrivilege");
  enable_privilege(L"SeDebugPrivilege");
  // The helper lives in the console session and opens this event by name, so
  // it must be in the global namespace.
  enable_privilege(L"SeCreateGlobalPrivilege");
  g_helper_stop_event =
      CreateEventW(nullptr, TRUE, FALSE, kHelperStopEventName);
  g_helper_detach_event =
      CreateEventW(nullptr, TRUE, FALSE, kHelperDetachEventName);
  if (g_helper_stop_event == nullptr || g_helper_detach_event == nullptr) {
    log_line("could not create helper stop events: " +
             std::to_string(GetLastError()));
  }
  std::string helper_error;
  if (!launch_helper(helper_error)) {
    log_line("helper launch failed: " + helper_error);
  }
  std::thread pipe_thread(pipe_server);
  // Follow the console session: "switch user", the logon screen of another
  // user, and the way back all move the console to a different session, and a
  // helper left behind in the old one cannot capture or inject anything (the
  // picture is black while audio keeps playing).
  uint64_t next_launch_attempt = 0;
  for (;;) {
    if (WaitForSingleObject(g_stop_event, 1000) == WAIT_OBJECT_0) break;
    if (!g_running.load()) break;
    const DWORD active = WTSGetActiveConsoleSessionId();
    if (active == 0xFFFFFFFF) {
      // No console to follow. Do not leave SERVICE_CONTROL_STOP disabled.
      if (g_controls_switching) report_helper_switched();
      continue;
    }
    const helper_state helper = query_helper();
    if (helper.running && active == helper.session) {
      // A secure-start request may have launched the helper while the
      // session-switch window was open; restore normal service controls.
      if (g_controls_switching) report_helper_switched();
      continue;
    }

    // The console moved if the helper was launched in a different session.
    // A helper that merely crashed in the current session keeps its core: the
    // relaunched helper adopts it instead of killing it.
    const bool session_moved =
        helper.session != 0xFFFFFFFF && helper.session != active;
    const uint64_t now = GetTickCount64();
    if (session_moved) next_launch_attempt = 0;
    if (now < next_launch_attempt) continue;
    next_launch_attempt = now + 5000;

    // The old core's graceful shutdown asks the SCM to stop this service.
    // That request belongs to the session switch, not to a user stop, so do
    // not accept it while the helper is being replaced.
    report_switching_helper();
    if (helper.running) {
      log_line("helper session " + std::to_string(helper.session) + " -> " +
               std::to_string(active) + ", restarting");
    }
    // On a session switch a dead helper may have left its core behind; stop
    // that core before the new session's helper starts. In the same session
    // the surviving core is adopted rather than killed.
    stop_helper(true);
    if (session_moved) terminate_stale_core();
    std::string relaunch_error;
    if (launch_helper(relaunch_error)) {
      log_line("helper running in session " + std::to_string(active));
    } else {
      log_line("helper relaunch failed: " + relaunch_error);
    }
    report_helper_switched();
  }
  g_running.store(false);
  // A service stop is not a console switch: let the core that asked for the
  // stop keep running, and take only the secure-desktop helper down. Signal
  // the helper first; its supervisor pokes its own pipe servers and exits.
  stop_helper(false);
  // ConnectNamedPipe blocks indefinitely; poke this service's pipe server so
  // it can observe running=false and the service can leave StopPending.
  poke_pipe(kPipeName);
  pipe_thread.join();
  if (g_helper_detach_event != nullptr) CloseHandle(g_helper_detach_event);
  if (g_helper_stop_event != nullptr) CloseHandle(g_helper_stop_event);
  if (g_stop_event != nullptr) CloseHandle(g_stop_event);
  log_line("service stopped");
  g_service_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(g_status, &g_service_status);
}

std::wstring program_data_service_dir() {
  wchar_t base[MAX_PATH * 2] = {};
  const DWORD length = GetEnvironmentVariableW(L"ProgramData", base,
                                               static_cast<DWORD>(std::size(base)));
  if (length == 0 || length >= std::size(base)) return std::wstring();
  return std::wstring(base) + L"\\moLink\\service";
}

std::wstring module_path() {
  wchar_t path[MAX_PATH * 4] = {};
  GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  return path;
}

bool file_exists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void stop_service_handle(SC_HANDLE service) {
  SERVICE_STATUS_PROCESS status{};
  DWORD bytes = 0;
  if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                            reinterpret_cast<LPBYTE>(&status),
                            sizeof(status), &bytes)) {
    return;
  }
  if (status.dwCurrentState == SERVICE_RUNNING) {
    SERVICE_STATUS stopped{};
    ControlService(service, SERVICE_CONTROL_STOP, &stopped);
  }
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&status),
                              sizeof(status), &bytes)) {
      return;
    }
    if (status.dwCurrentState == SERVICE_STOPPED) return;
    if (status.dwCurrentState == SERVICE_STOP_PENDING &&
        status.dwProcessId != 0) {
      HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE,
                                   status.dwProcessId);
      if (process != nullptr) {
        TerminateProcess(process, 0);
        CloseHandle(process);
      }
    }
    Sleep(100);
  }
}

void install_error(const wchar_t* stage, DWORD code) {
  std::fwprintf(stderr, L"molink-service: %ls failed (error %lu)\n", stage,
                code);
}

bool install_service() {
  const std::wstring source = module_path();
  const std::size_t slash = source.find_last_of(L"\\/");
  const std::wstring install_dir = program_data_service_dir();
  if (source.empty() || install_dir.empty()) return false;
  // An overwrite install must not try to replace a binary that is still
  // mapped by the running service. Stop it (and terminate a stuck StopPending
  // process) before copying.
  {
    SC_HANDLE manager =
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager != nullptr) {
      SC_HANDLE existing = OpenServiceW(manager, kServiceName,
                                        SERVICE_STOP | SERVICE_QUERY_STATUS);
      if (existing != nullptr) {
        stop_service_handle(existing);
        CloseServiceHandle(existing);
      }
      CloseServiceHandle(manager);
    }
  }
  const std::size_t parent_cut = install_dir.find_last_of(L"\\");
  if (parent_cut != std::wstring::npos) {
    CreateDirectoryW(install_dir.substr(0, parent_cut).c_str(), nullptr);
  }
  CreateDirectoryW(install_dir.c_str(), nullptr);
  const std::wstring target = install_dir + L"\\molink-service.exe";
  if (_wcsicmp(source.c_str(), target.c_str()) != 0) {
    if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
      install_error(L"CopyFile", GetLastError());
      return false;
    }
    if (slash != std::wstring::npos) {
      const std::wstring helper_src = source.substr(0, slash + 1) +
                                      L"molink-helper.exe";
      const std::wstring helper_dst = install_dir + L"\\molink-helper.exe";
      CopyFileW(helper_src.c_str(), helper_dst.c_str(), FALSE);
    }
  }
  // The helper runs from %ProgramData%\moLink\service but the core normally
  // stays next to the packaged app. Record that location so the supervisor
  // does not have to guess (and so a package outside Program Files works).
  if (slash != std::wstring::npos) {
    const std::wstring core_src =
        source.substr(0, slash + 1) + L"molink-core.exe";
    if (file_exists(core_src)) {
      std::ofstream hint((install_dir + L"\\core.path").c_str(),
                         std::ios::trunc);
      if (hint) hint << wide_to_utf8(core_src.c_str()) << "\n";
    }
  }
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (manager == nullptr) {
    install_error(L"OpenSCManager", GetLastError());
    return false;
  }
  SC_HANDLE service = CreateServiceW(
      manager, kServiceName, L"moLink session helper",
      SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START,
      SERVICE_ERROR_NORMAL, target.c_str(), nullptr, nullptr, nullptr, nullptr,
      nullptr);
  bool update_config = true;
  if (service == nullptr) {
    const DWORD create_error = GetLastError();
    if (create_error != ERROR_SERVICE_EXISTS) {
      install_error(L"CreateService", create_error);
    } else {
      // Installs up to 0.6.11 restrict the DACL to enumerate/start/stop for
      // administrators as well as interactive users, so SERVICE_ALL_ACCESS
      // and even SERVICE_CHANGE_CONFIG are denied. Ask for the full set
      // first, then fall back to the rights those installs grant. The binary
      // path is fixed, so skipping the config rewrite is harmless.
      service = OpenServiceW(manager, kServiceName,
                             SERVICE_CHANGE_CONFIG | SERVICE_START |
                                 SERVICE_QUERY_STATUS);
      if (service == nullptr) {
        service = OpenServiceW(manager, kServiceName,
                               SERVICE_START | SERVICE_QUERY_STATUS);
        update_config = false;
      }
      if (service == nullptr) {
        install_error(L"OpenService", GetLastError());
      }
    }
  }
  if (service == nullptr) {
    CloseServiceHandle(manager);
    return false;
  }
  if (update_config) {
    ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_DEMAND_START,
                         SERVICE_NO_CHANGE, target.c_str(), nullptr, nullptr,
                         nullptr, nullptr, nullptr, L"moLink session helper");
  }
  // Allow services (this one and no other caller class) to generate the
  // secure attention sequence, which is how the app's Ctrl+Alt+Del button
  // reaches the lock screen. Existing values 2/3 are left alone.
  HKEY policy = nullptr;
  if (RegCreateKeyExW(
          HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
          0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &policy,
          nullptr) == ERROR_SUCCESS) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS queried =
        RegQueryValueExW(policy, L"SoftwareSASGeneration", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &size);
    if (queried != ERROR_SUCCESS || type != REG_DWORD || value == 0) {
      value = 1;
      if (RegSetValueExW(policy, L"SoftwareSASGeneration", 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&value),
                         sizeof(value)) == ERROR_SUCCESS) {
        log_line("enabled SoftwareSASGeneration for services");
      }
    }
    RegCloseKey(policy);
  }

  // Let the interactive user start/stop the service from the app without an
  // elevation prompt every time. Install and uninstall still require admin.
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;CCLCSWRPWPDTLOCRRC;;;IU)",
          SDDL_REVISION_1, &descriptor, nullptr)) {
    SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
  }
  bool started = StartServiceW(service, 0, nullptr) != 0;
  if (!started) {
    const DWORD start_error = GetLastError();
    started = start_error == ERROR_SERVICE_ALREADY_RUNNING;
    if (!started) install_error(L"StartService", start_error);
  }
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  return started;
}

bool uninstall_service() {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (manager == nullptr) return false;
  SC_HANDLE service = OpenServiceW(manager, kServiceName,
                                   DELETE | SERVICE_STOP |
                                       SERVICE_QUERY_STATUS);
  if (service == nullptr) {
    CloseServiceHandle(manager);
    return false;
  }
  stop_service_handle(service);
  const bool ok = DeleteService(service) != 0;
  CloseServiceHandle(service);
  CloseServiceHandle(manager);
  const std::wstring dir = program_data_service_dir();
  if (!dir.empty()) {
    DeleteFileW((dir + L"\\molink-service.exe").c_str());
    DeleteFileW((dir + L"\\molink-helper.exe").c_str());
    RemoveDirectoryW(dir.c_str());
  }
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
