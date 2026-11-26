#include "terminal/pty.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#include "diag_log.h"
#include "terminal/helper_protocol.h"

#if !defined(_WIN32)

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <cctype>

namespace molink {
namespace {

/// Split a command line into argv with single/double quote and backslash
/// support. `pty_spawn` takes a command, not just a path, so the tests can ask
/// for `/bin/sh -c 'echo hello'` without a second shell around the core.
std::vector<std::string> split_command(const std::string& command) {
  std::vector<std::string> args;
  std::string current;
  bool in_single = false;
  bool in_double = false;
  bool started = false;
  for (std::size_t i = 0; i < command.size(); ++i) {
    const char c = command[i];
    if (in_single) {
      if (c == '\'') {
        in_single = false;
      } else {
        current.push_back(c);
      }
      continue;
    }
    if (in_double) {
      if (c == '"') {
        in_double = false;
      } else if (c == '\\' && i + 1 < command.size() &&
                 (command[i + 1] == '"' || command[i + 1] == '\\' ||
                  command[i + 1] == '$' || command[i + 1] == '`')) {
        current.push_back(command[++i]);
      } else {
        current.push_back(c);
      }
      continue;
    }
    if (c == '\'') {
      in_single = true;
      started = true;
    } else if (c == '"') {
      in_double = true;
      started = true;
    } else if (c == '\\' && i + 1 < command.size()) {
      current.push_back(command[++i]);
      started = true;
    } else if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (started) {
        args.push_back(current);
        current.clear();
        started = false;
      }
    } else {
      current.push_back(c);
      started = true;
    }
  }
  if (started) args.push_back(current);
  return args;
}

int clamp_dimension(int value, int fallback) {
  if (value <= 0) return fallback;
  if (value > 1000) return 1000;
  return value;
}

void apply_winsize(int fd, int cols, int rows) {
  struct winsize size = {};
  size.ws_col = static_cast<unsigned short>(cols);
  size.ws_row = static_cast<unsigned short>(rows);
  ::ioctl(fd, TIOCSWINSZ, &size);
}

void sleep_ms(long ms) {
  struct timespec request = {};
  request.tv_sec = ms / 1000;
  request.tv_nsec = (ms % 1000) * 1000 * 1000;
  ::nanosleep(&request, nullptr);
}

int status_exit_code(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

/// Login name for `term.spawn`, so a real-machine log shows which account the
/// shell inherited on every platform. The POSIX path is already the right one.
std::string current_user_name() {
  for (const char* name : {"USER", "LOGNAME"}) {
    const char* value = std::getenv(name);
    if (value != nullptr && value[0] != '\0') return value;
  }
  return "?";
}

}  // namespace

bool pty_supported() { return true; }

bool pty_valid(const pty_handle& handle) { return handle.fd >= 0; }

pty_handle pty_spawn(const std::string& command, int cols, int rows,
                     std::string& err) {
  pty_handle handle;
  err.clear();
  cols = clamp_dimension(cols, 80);
  rows = clamp_dimension(rows, 24);

  const std::string line = command.empty() ? pty_default_shell() : command;
  std::vector<std::string> args = split_command(line);
  if (args.empty()) {
    err = "empty terminal command";
    return handle;
  }
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (std::string& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);

  const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0) {
    err = std::string("posix_openpt: ") + std::strerror(errno);
    return handle;
  }
  if (::grantpt(master) != 0) {
    err = std::string("grantpt: ") + std::strerror(errno);
    ::close(master);
    return handle;
  }
  if (::unlockpt(master) != 0) {
    err = std::string("unlockpt: ") + std::strerror(errno);
    ::close(master);
    return handle;
  }
  const char* slave_path = ::ptsname(master);
  if (slave_path == nullptr) {
    err = std::string("ptsname: ") + std::strerror(errno);
    ::close(master);
    return handle;
  }
  // Copy before fork: only the forked child needs the string, and this keeps
  // the child's path free of allocations.
  const std::string slave_name = slave_path;

  // The child signals after the slave side is open, so the parent's initial
  // window size cannot race the child's own TIOCSCTTY setup. Without this a
  // resize issued right after pty_spawn could be overwritten by the child's
  // initial 80x24.
  int ready_pipe[2] = {-1, -1};
  if (::pipe(ready_pipe) != 0) {
    err = std::string("pipe: ") + std::strerror(errno);
    ::close(master);
    return handle;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    err = std::string("fork: ") + std::strerror(errno);
    ::close(ready_pipe[0]);
    ::close(ready_pipe[1]);
    ::close(master);
    return handle;
  }
  if (pid == 0) {
    // Child: a fresh session with the slave side as its controlling terminal.
    // Every branch below exits without unwinding, so no parent state leaks in.
    ::close(ready_pipe[0]);
    ::close(master);
    if (::setsid() < 0) ::_exit(126);
    const int slave = ::open(slave_name.c_str(), O_RDWR);
    if (slave < 0) ::_exit(126);
    ::ioctl(slave, TIOCSCTTY, 0);
    const char ready = 1;
    const ssize_t ready_written = ::write(ready_pipe[1], &ready, 1);
    (void)ready_written;
    ::close(ready_pipe[1]);
    if (::dup2(slave, STDIN_FILENO) < 0 ||
        ::dup2(slave, STDOUT_FILENO) < 0 ||
        ::dup2(slave, STDERR_FILENO) < 0) {
      ::_exit(126);
    }
    if (slave > STDERR_FILENO) ::close(slave);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
  }

  ::close(ready_pipe[1]);
  char ready = 0;
  ssize_t ready_bytes = 0;
  do {
    ready_bytes = ::read(ready_pipe[0], &ready, 1);
  } while (ready_bytes < 0 && errno == EINTR);
  ::close(ready_pipe[0]);

  // The master side is non-blocking so a service poll can drain it without
  // holding up the event loop.
  const int flags = ::fcntl(master, F_GETFL, 0);
  if (flags >= 0) ::fcntl(master, F_SETFL, flags | O_NONBLOCK);
  apply_winsize(master, cols, rows);

  handle.fd = master;
  handle.pid = pid;
  // POSIX has no service-account problem: core and shell already share the
  // login user. Record it anyway so both backends appear in the same log.
  diag().log(diag_level::info, "term.spawn", std::string(),
             {{"backend", "direct"},
              {"user", current_user_name()},
              {"session_id", "-"}});
  return handle;
}

bool pty_resize(pty_handle& handle, int cols, int rows) {
  if (handle.fd < 0) return false;
  cols = clamp_dimension(cols, 80);
  rows = clamp_dimension(rows, 24);
  struct winsize size = {};
  size.ws_col = static_cast<unsigned short>(cols);
  size.ws_row = static_cast<unsigned short>(rows);
  if (::ioctl(handle.fd, TIOCSWINSZ, &size) != 0) return false;
  if (handle.pid > 0 && !handle.reaped) {
    // The child called setsid(), so its pid is also its process-group id.
    if (::kill(-handle.pid, SIGWINCH) != 0) {
      ::kill(handle.pid, SIGWINCH);
    }
  }
  return true;
}

int pty_write(pty_handle& handle, const void* data, std::size_t len) {
  if (handle.fd < 0) return -1;
  const char* bytes = static_cast<const char*>(data);
  std::size_t written = 0;
  while (written < len) {
    const ssize_t n = ::write(handle.fd, bytes + written, len - written);
    if (n > 0) {
      written += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (written > 0) return static_cast<int>(written);
    return -1;
  }
  return static_cast<int>(written);
}

int pty_read(pty_handle& handle, void* buffer, std::size_t len) {
  if (handle.fd < 0) return -1;
  for (;;) {
    const ssize_t n = ::read(handle.fd, buffer, len);
    if (n < 0 && errno == EINTR) continue;
    return static_cast<int>(n);
  }
}

bool pty_poll_exit(pty_handle& handle, int& exit_code) {
  if (handle.reaped) {
    exit_code = handle.exit_code;
    return handle.pid > 0;
  }
  if (handle.pid <= 0) return false;
  int status = 0;
  const pid_t result = ::waitpid(handle.pid, &status, WNOHANG);
  if (result == handle.pid) {
    handle.reaped = true;
    handle.exit_code = status_exit_code(status);
    exit_code = handle.exit_code;
    return true;
  }
  if (result < 0 && errno == ECHILD) {
    // Someone else reaped it; the process is gone either way.
    handle.reaped = true;
    handle.exit_code = -1;
    exit_code = handle.exit_code;
    return true;
  }
  return false;
}

void pty_kill(pty_handle& handle) {
  if (handle.pid > 0 && !handle.reaped) {
    ::kill(-handle.pid, SIGHUP);
    ::kill(handle.pid, SIGHUP);
    int code = -1;
    bool gone = false;
    for (int i = 0; i < 20 && !gone; ++i) {
      if (pty_poll_exit(handle, code)) {
        gone = true;
        break;
      }
      sleep_ms(5);
    }
    if (!gone) {
      ::kill(-handle.pid, SIGKILL);
      ::kill(handle.pid, SIGKILL);
      for (int i = 0; i < 200 && !gone; ++i) {
        if (pty_poll_exit(handle, code)) {
          gone = true;
          break;
        }
        sleep_ms(5);
      }
    }
    if (!gone) {
      int status = 0;
      if (::waitpid(handle.pid, &status, 0) == handle.pid) {
        handle.reaped = true;
        handle.exit_code = status_exit_code(status);
      }
    }
  }
  if (handle.fd >= 0) {
    ::close(handle.fd);
    handle.fd = -1;
  }
}

std::string pty_default_shell() {
  const char* shell = std::getenv("SHELL");
  if (shell != nullptr && shell[0] != '\0') return shell;
  return "/bin/sh";
}

}  // namespace molink

#else  // _WIN32

// windows.h must stay out of terminal/pty.h: it defines min()/max() as macros
// and MSVC then rejects std::min with C2589. NOMINMAX keeps this TU safe too.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// ConPTY is a Windows 10 1809 (NTDDI_WIN10_RS5) API. Defining the target in
// this TU keeps the declarations and import available with MSVC, whose SDK
// otherwise defaults to an older minimum. A toolchain that passes its own
// _WIN32_WINNT (the Win7 x86 build passes 0x0601) wins, so that binary does
// not grow an import Win7 cannot resolve; pty_supported() is false there and
// the UI keeps its "unsupported" message.
#if !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif
#if (_WIN32_WINNT >= 0x0A00) && !defined(NTDDI_VERSION)
// The pseudoconsole declarations are gated on the RS5 service sub-version,
// which _WIN32_WINNT alone does not carry.
#define NTDDI_VERSION 0x0A000006  // NTDDI_WIN10_RS5
#endif
#include <windows.h>
#include <wtsapi32.h>

#if defined(NTDDI_VERSION) && (NTDDI_VERSION >= 0x0A000006)
#define MOLINK_HAS_CONPTY 1
#else
#define MOLINK_HAS_CONPTY 0
#endif

namespace molink {
namespace {

int clamp_dimension(int value, int fallback) {
  if (value <= 0) return fallback;
  if (value > 1000) return 1000;
  return value;
}

std::string win_error(const char* call, DWORD code) {
  return std::string(call) + " failed (win32 error " + std::to_string(code) +
         ")";
}

#if MOLINK_HAS_CONPTY
std::wstring widen_utf8(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int length = static_cast<int>(text.size());
  const int needed =
      ::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, 0, text.data(), length, out.data(),
                            needed) <= 0) {
    return std::wstring();
  }
  return out;
}
#endif  // MOLINK_HAS_CONPTY

std::string utf8_from_wide(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int length = static_cast<int>(text.size());
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length,
                                           nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string out(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), length, out.data(), needed,
                        nullptr, nullptr);
  return out;
}

DWORD current_session_id() {
  DWORD session = 0;
  if (::ProcessIdToSessionId(::GetCurrentProcessId(), &session)) return session;
  return 0;
}

std::wstring wts_session_string(DWORD session, WTS_INFO_CLASS info) {
  LPWSTR buffer = nullptr;
  DWORD bytes = 0;
  if (!::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, info,
                                     &buffer, &bytes) ||
      buffer == nullptr || bytes == 0) {
    return std::wstring();
  }
  std::wstring value(buffer);
  ::WTSFreeMemory(buffer);
  return value;
}

std::wstring current_user_name_w() {
  wchar_t buffer[256] = {};
  DWORD length = static_cast<DWORD>(std::size(buffer));
  if (!::GetUserNameW(buffer, &length) || length == 0) return std::wstring();
  return std::wstring(buffer, length > 0 ? length - 1 : 0);
}

/// Whether this process already runs as the user logged into the active
/// console session. Only then may the core start a shell itself: a
/// service-hosted core (SYSTEM, or an elevated service account) must go
/// through the helper, because inheriting its own token is exactly the bug
/// this bridge fixes.
bool running_as_console_user() {
  const DWORD session = current_session_id();
  if (session == 0) return false;  // services run in session 0
  const DWORD active = ::WTSGetActiveConsoleSessionId();
  if (active == 0xFFFFFFFF || session != active) return false;

  const std::wstring session_user =
      wts_session_string(active, WTSUserName);
  if (session_user.empty()) return false;
  const std::wstring session_domain =
      wts_session_string(active, WTSDomainName);

  std::wstring current_domain;
  std::wstring current_user = current_user_name_w();
  const std::size_t slash = current_user.find(L'\\');
  if (slash != std::wstring::npos) {
    current_domain = current_user.substr(0, slash);
    current_user = current_user.substr(slash + 1);
  }
  if (current_user.empty() ||
      ::_wcsicmp(current_user.c_str(), session_user.c_str()) != 0) {
    return false;
  }
  if (!session_domain.empty() && !current_domain.empty() &&
      ::_wcsicmp(session_domain.c_str(), current_domain.c_str()) != 0) {
    return false;
  }
  return true;
}

HANDLE open_helper_pipe(DWORD wait_ms) {
  if (wait_ms != 0) {
    ::WaitNamedPipeW(terminal_bridge::kPipeName, wait_ms);
  }
  HANDLE pipe = ::CreateFileW(terminal_bridge::kPipeName,
                              GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return nullptr;
  DWORD mode = PIPE_READMODE_MESSAGE;
  if (!::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
    ::CloseHandle(pipe);
    return nullptr;
  }
  return pipe;
}

bool write_pipe_message(HANDLE pipe, const void* data, DWORD len) {
  DWORD written = 0;
  return ::WriteFile(pipe, data, len, &written, nullptr) != 0 &&
         written == len;
}

std::string reply_field(const std::string& line, const std::string& key) {
  const std::string needle = " " + key + "=";
  const std::size_t pos = line.find(needle);
  if (pos == std::string::npos) return std::string();
  const std::size_t start = pos + needle.size();
  const std::size_t end = line.find(' ', start);
  return line.substr(start, end == std::string::npos ? std::string::npos
                                                     : end - start);
}

/// Text after "<key>=" up to the end of the line; user names may contain
/// spaces, so the helper puts this field last.
std::string reply_rest(const std::string& line, const std::string& key) {
  const std::string needle = " " + key + "=";
  const std::size_t pos = line.find(needle);
  if (pos == std::string::npos) return std::string();
  return line.substr(pos + needle.size());
}

struct helper_reply {
  bool attempted = false;
  HANDLE pipe = nullptr;
  int pid = -1;
  unsigned session = 0;
  std::string user;
  std::string reason;
  std::string message;
};

/// Ask the helper for a PTY owned by the console user. On success `pipe` is
/// the live stream connection; on failure `reason` explains why the helper
/// could not serve it (and `attempted` tells "no helper at all" from "helper
/// replied with an error").
helper_reply try_helper_spawn(const std::string& command, int cols,
                              int rows) {
  helper_reply reply;
  HANDLE pipe = open_helper_pipe(150);
  if (pipe == nullptr) {
    reply.reason = "no_session_helper";
    return reply;
  }
  reply.attempted = true;

  std::string request = std::string(terminal_bridge::kOpenPrefix) + " " +
                        std::to_string(cols) + " " + std::to_string(rows);
  if (!command.empty()) request += " " + command;
  if (!write_pipe_message(pipe, request.data(),
                          static_cast<DWORD>(request.size()))) {
    reply.reason = "helper_write_failed";
    reply.message = win_error("WriteFile", ::GetLastError());
    ::CloseHandle(pipe);
    return reply;
  }

  std::vector<char> buffer(4096);
  DWORD read = 0;
  if (!::ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                  nullptr) ||
      read == 0) {
    reply.reason = "helper_reply_failed";
    reply.message = win_error("ReadFile", ::GetLastError());
    ::CloseHandle(pipe);
    return reply;
  }
  const std::string line(buffer.data(), read);
  if (line.rfind(terminal_bridge::kOkPrefix, 0) == 0) {
    reply.pid = std::atoi(reply_field(line, "pid").c_str());
    reply.session = static_cast<unsigned>(
        std::strtoul(reply_field(line, "session").c_str(), nullptr, 10));
    reply.user = reply_rest(line, "user");
    if (reply.user.empty()) reply.user = "?";
    reply.pipe = pipe;
    return reply;
  }
  if (line.rfind(terminal_bridge::kErrorPrefix, 0) == 0) {
    reply.reason = reply_field(line, "reason");
    if (reply.reason.empty()) reply.reason = "helper_error";
    reply.message = reply_rest(line, "message");
  } else {
    // An older helper treats `term-open` as an unknown command and answers
    // with its JSON error. Treat that as "bridge unsupported", never as a
    // reason to start a shell with the service token.
    reply.reason = "helper_unsupported";
    reply.message = line.substr(0, 200);
  }
  ::CloseHandle(pipe);
  return reply;
}

void put_u32(unsigned char* out, std::uint32_t value) {
  out[0] = static_cast<unsigned char>(value & 0xff);
  out[1] = static_cast<unsigned char>((value >> 8) & 0xff);
  out[2] = static_cast<unsigned char>((value >> 16) & 0xff);
  out[3] = static_cast<unsigned char>((value >> 24) & 0xff);
}

std::uint32_t get_u32(const unsigned char* in) {
  return static_cast<std::uint32_t>(in[0]) |
         (static_cast<std::uint32_t>(in[1]) << 8) |
         (static_cast<std::uint32_t>(in[2]) << 16) |
         (static_cast<std::uint32_t>(in[3]) << 24);
}

}  // namespace

/// One terminal session. A copy of the handle shares this state so a stray
/// copy cannot double-close the pipes; `mode` selects the ConPTY backend or
/// the helper bridge.
struct pty_impl {
  enum class kind { conpty, helper };
  kind mode = kind::conpty;

  // Direct ConPTY backend.
#if MOLINK_HAS_CONPTY
  HPCON pc = nullptr;
  HANDLE in_write = nullptr;  // viewer -> child (the child's stdin)
  HANDLE out_read = nullptr;  // child -> viewer (the child's stdout/stderr)
  HANDLE process = nullptr;
  bool killed = false;
#endif

  // Helper bridge backend.
  HANDLE helper_pipe = nullptr;
  bool helper_exited = false;
  int helper_exit_code = -1;
  /// Reused frame buffer so a keystroke does not allocate.
  std::vector<unsigned char> helper_out;

  ~pty_impl() { release(); }

  void release() {
    if (mode == kind::helper) {
      if (helper_pipe != nullptr) {
        ::CloseHandle(helper_pipe);
        helper_pipe = nullptr;
      }
      return;
    }
#if MOLINK_HAS_CONPTY
    if (process != nullptr) {
      // A process that was already reaped still has an open handle;
      // TerminateProcess on it simply fails.
      ::TerminateProcess(process, 1);
      ::WaitForSingleObject(process, 500);
      ::CloseHandle(process);
      process = nullptr;
    }
    if (pc != nullptr) {
      ::ClosePseudoConsole(pc);
      pc = nullptr;
    }
    if (in_write != nullptr) {
      ::CloseHandle(in_write);
      in_write = nullptr;
    }
    if (out_read != nullptr) {
      ::CloseHandle(out_read);
      out_read = nullptr;
    }
#endif
  }
};

bool pty_supported() {
#if MOLINK_HAS_CONPTY
  return true;
#else
  return false;
#endif
}

bool pty_valid(const pty_handle& handle) {
  if (handle.impl == nullptr) return false;
  if (handle.impl->mode == pty_impl::kind::helper) {
    return handle.impl->helper_pipe != nullptr;
  }
#if MOLINK_HAS_CONPTY
  return handle.impl->pc != nullptr;
#else
  return false;
#endif
}

pty_handle pty_spawn(const std::string& command, int cols, int rows,
                     std::string& err) {
  pty_handle handle;
  err.clear();
  cols = clamp_dimension(cols, 80);
  rows = clamp_dimension(rows, 24);

  // Preferred path: the helper runs in the interactive session and can start
  // the shell with the console user's token, which a service-hosted core
  // cannot do by itself.
  helper_reply helper = try_helper_spawn(command, cols, rows);
  if (helper.pipe != nullptr) {
    auto impl = std::make_shared<pty_impl>();
    impl->mode = pty_impl::kind::helper;
    impl->helper_pipe = helper.pipe;
    handle.impl = impl;
    handle.pid = helper.pid;
    diag().log(diag_level::info, "term.spawn", std::string(),
               {{"backend", "helper"},
                {"user", helper.user},
                {"session_id", std::to_string(helper.session)}});
    return handle;
  }

  // No usable helper. Starting a ConPTY here is only correct when this core
  // already runs as the logged-in user (a shell that launched the core
  // directly, or a core the helper started as that user). Otherwise the
  // inherited token is the service account, and spawning would silently
  // elevate the remote terminal -- report the failure instead.
  const bool direct_ok = running_as_console_user();
  const std::string user = utf8_from_wide(current_user_name_w());
  const std::string session_id = std::to_string(current_session_id());
  if (!direct_ok) {
    std::string reason =
        helper.reason.empty() ? "no_session_helper" : helper.reason;
    err = "terminal_unavailable: " + reason +
          " (core runs as " + (user.empty() ? "?" : user) + " in session " +
          session_id +
          ", not the console user; refusing to start a shell as the service "
          "account)";
    if (!helper.message.empty()) err += ": " + helper.message;
    diag().log(diag_level::warn, "term.spawn", err,
               {{"backend", "unavailable"},
                {"user", user.empty() ? "?" : user},
                {"session_id", session_id},
                {"reason", helper.reason.empty() ? "no_session_helper"
                                                 : helper.reason}});
    return handle;
  }

#if !MOLINK_HAS_CONPTY
  err =
      "terminal_unavailable: ConPTY requires Windows 10 1809 or newer";
  return handle;
#else
  const std::string line = command.empty() ? pty_default_shell() : command;
  if (line.empty()) {
    err = "empty terminal command";
    return handle;
  }
  std::wstring command_line = widen_utf8(line);
  if (command_line.empty()) {
    err = "terminal command is not valid UTF-8";
    return handle;
  }
  // A default shell is a bare executable path and may contain spaces, so pass
  // it as lpApplicationName. A caller-supplied command line is parsed by
  // CreateProcess like any other.
  std::wstring application = command.empty() ? command_line : std::wstring();

  HANDLE in_read = nullptr;
  HANDLE in_write = nullptr;
  HANDLE out_read = nullptr;
  HANDLE out_write = nullptr;
  auto close_handle = [](HANDLE& value) {
    if (value != nullptr) {
      ::CloseHandle(value);
      value = nullptr;
    }
  };
  if (!::CreatePipe(&in_read, &in_write, nullptr, 0) ||
      !::CreatePipe(&out_read, &out_write, nullptr, 0)) {
    err = win_error("CreatePipe", ::GetLastError());
    close_handle(in_read);
    close_handle(in_write);
    close_handle(out_read);
    close_handle(out_write);
    return handle;
  }

  HPCON pc = nullptr;
  const COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
  const HRESULT console_created =
      ::CreatePseudoConsole(size, in_read, out_write, 0, &pc);
  if (FAILED(console_created) || pc == nullptr) {
    err = "CreatePseudoConsole failed (hr=" +
          std::to_string(static_cast<long long>(console_created)) + ")";
    if (pc != nullptr) ::ClosePseudoConsole(pc);
    close_handle(in_read);
    close_handle(in_write);
    close_handle(out_read);
    close_handle(out_write);
    return handle;
  }
  // The pseudoconsole holds its own references to these two; the viewer keeps
  // the other ends.
  close_handle(in_read);
  close_handle(out_write);

  auto impl = std::make_shared<pty_impl>();
  impl->mode = pty_impl::kind::conpty;
  impl->pc = pc;
  impl->in_write = in_write;
  impl->out_read = out_read;

  SIZE_T attribute_bytes = 0;
  ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
  std::vector<unsigned char> attribute_storage(attribute_bytes);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
      attribute_storage.data());
  if (attribute_bytes == 0 ||
      !::InitializeProcThreadAttributeList(attributes, 1, 0,
                                           &attribute_bytes)) {
    err = win_error("InitializeProcThreadAttributeList", ::GetLastError());
    impl->release();
    return handle;
  }
  if (!::UpdateProcThreadAttribute(attributes, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pc,
                                   sizeof(pc), nullptr, nullptr)) {
    err = win_error("UpdateProcThreadAttribute", ::GetLastError());
    ::DeleteProcThreadAttributeList(attributes);
    impl->release();
    return handle;
  }

  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes;
  PROCESS_INFORMATION process_info = {};
  // EXTENDED_STARTUPINFO_PRESENT is required for lpAttributeList to be read.
  const BOOL spawned = ::CreateProcessW(
      application.empty() ? nullptr : application.data(), command_line.data(),
      nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
      &startup.StartupInfo, &process_info);
  const DWORD spawn_error = ::GetLastError();
  ::DeleteProcThreadAttributeList(attributes);
  if (!spawned) {
    err = win_error("CreateProcess", spawn_error);
    impl->release();
    return handle;
  }
  ::CloseHandle(process_info.hThread);
  impl->process = process_info.hProcess;
  handle.impl = impl;
  handle.pid = static_cast<int>(process_info.dwProcessId);
  diag().log(diag_level::info, "term.spawn", std::string(),
             {{"backend", "direct"},
              {"user", user.empty() ? "?" : user},
              {"session_id", session_id}});
  return handle;
#endif
}

bool pty_resize(pty_handle& handle, int cols, int rows) {
  if (handle.impl == nullptr) return false;
  cols = clamp_dimension(cols, 80);
  rows = clamp_dimension(rows, 24);
  if (handle.impl->mode == pty_impl::kind::helper) {
    if (handle.impl->helper_pipe == nullptr) return false;
    unsigned char frame[1 + terminal_bridge::kResizePayload] = {};
    frame[0] = static_cast<unsigned char>(terminal_bridge::to_helper::resize);
    put_u32(frame + 1, static_cast<std::uint32_t>(cols));
    put_u32(frame + 5, static_cast<std::uint32_t>(rows));
    return write_pipe_message(handle.impl->helper_pipe, frame,
                              static_cast<DWORD>(sizeof(frame)));
  }
#if !MOLINK_HAS_CONPTY
  return false;
#else
  if (handle.impl->pc == nullptr) return false;
  const COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
  return SUCCEEDED(::ResizePseudoConsole(handle.impl->pc, size));
#endif
}

int pty_write(pty_handle& handle, const void* data, std::size_t len) {
  if (handle.impl == nullptr) return -1;
  const char* bytes = static_cast<const char*>(data);
  if (handle.impl->mode == pty_impl::kind::helper) {
    if (handle.impl->helper_pipe == nullptr ||
        handle.impl->helper_exited) {
      return -1;
    }
    std::vector<unsigned char>& frame = handle.impl->helper_out;
    std::size_t written = 0;
    while (written < len) {
      const std::size_t chunk = (std::min)(
          len - written, terminal_bridge::kMaxFramePayload);
      frame.resize(1 + chunk);
      frame[0] = static_cast<unsigned char>(terminal_bridge::to_helper::input);
      std::memcpy(frame.data() + 1, bytes + written, chunk);
      if (!write_pipe_message(handle.impl->helper_pipe, frame.data(),
                              static_cast<DWORD>(frame.size()))) {
        if (written > 0) return static_cast<int>(written);
        errno = EIO;
        return -1;
      }
      written += chunk;
    }
    return static_cast<int>(written);
  }
#if !MOLINK_HAS_CONPTY
  return -1;
#else
  if (handle.impl->in_write == nullptr) return -1;
  std::size_t written = 0;
  while (written < len) {
    // Anonymous pipes have no O_NONBLOCK; a full input buffer blocks this
    // write. Terminal input is keystrokes and bounded pastes, so the event
    // loop does not sit here for long.
    const DWORD chunk = static_cast<DWORD>((std::min)(
        len - written, static_cast<std::size_t>(64u * 1024u)));
    DWORD count = 0;
    if (!::WriteFile(handle.impl->in_write, bytes + written, chunk, &count,
                     nullptr)) {
      const DWORD code = ::GetLastError();
      if (code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA) {
        return written > 0 ? static_cast<int>(written) : -1;
      }
      if (written > 0) return static_cast<int>(written);
      errno = EIO;
      return -1;
    }
    if (count == 0) break;
    written += count;
  }
  return static_cast<int>(written);
#endif
}

namespace {

/// Read one helper message. Returns payload bytes for an output frame, 0 at
/// end of stream, -1 with EAGAIN when nothing is buffered and -1 with EIO on
/// a broken/oversized message. An exit frame records the code and reports
/// EAGAIN; pty_poll_exit() then observes it.
int helper_read_frame(pty_impl& impl, void* buffer, std::size_t len) {
  if (impl.helper_pipe == nullptr) return -1;
  DWORD available = 0;
  if (!::PeekNamedPipe(impl.helper_pipe, nullptr, 0, nullptr, &available,
                       nullptr)) {
    impl.helper_exited = true;
    if (impl.helper_exit_code < 0) impl.helper_exit_code = 1;
    return 0;
  }
  if (available == 0) {
    errno = EAGAIN;
    return -1;
  }
  if (len < 2) {
    errno = EAGAIN;
    return -1;
  }
  const DWORD chunk = static_cast<DWORD>((std::min)(
      len, static_cast<std::size_t>(0xFFFFFFFFu)));
  DWORD count = 0;
  if (!::ReadFile(impl.helper_pipe, buffer, chunk, &count, nullptr)) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED ||
        code == ERROR_INVALID_HANDLE) {
      impl.helper_exited = true;
      if (impl.helper_exit_code < 0) impl.helper_exit_code = 1;
      return 0;
    }
    if (code == ERROR_MORE_DATA) {
      // Larger than the caller's buffer (never produced by this helper).
      // Drain the rest of the message so the stream stays aligned.
      unsigned char scratch[4096];
      for (;;) {
        DWORD more = 0;
        if (::ReadFile(impl.helper_pipe, scratch,
                       static_cast<DWORD>(sizeof(scratch)), &more, nullptr)) {
          break;
        }
        if (::GetLastError() != ERROR_MORE_DATA) break;
      }
    }
    errno = EIO;
    return -1;
  }
  if (count == 0) {
    impl.helper_exited = true;
    if (impl.helper_exit_code < 0) impl.helper_exit_code = 1;
    return 0;
  }
  const auto* bytes = static_cast<const unsigned char*>(buffer);
  const auto kind = static_cast<terminal_bridge::from_helper>(bytes[0]);
  if (kind == terminal_bridge::from_helper::exit) {
    if (count >= 1 + terminal_bridge::kExitPayload) {
      const std::uint32_t raw = get_u32(bytes + 1);
      impl.helper_exit_code = static_cast<int>(raw);
    } else {
      impl.helper_exit_code = 0;
    }
    impl.helper_exited = true;
    errno = EAGAIN;
    return -1;
  }
  if (kind != terminal_bridge::from_helper::output) {
    errno = EIO;
    return -1;
  }
  const std::size_t payload = static_cast<std::size_t>(count) - 1;
  std::memmove(buffer, bytes + 1, payload);
  return static_cast<int>(payload);
}

/// Consume buffered helper messages without a caller buffer. Used while
/// stopping, where output no longer matters but the exit frame does.
void helper_drain(pty_impl& impl) {
  if (impl.helper_pipe == nullptr || impl.helper_exited) return;
  for (;;) {
    DWORD available = 0;
    if (!::PeekNamedPipe(impl.helper_pipe, nullptr, 0, nullptr, &available,
                         nullptr)) {
      impl.helper_exited = true;
      if (impl.helper_exit_code < 0) impl.helper_exit_code = 1;
      return;
    }
    if (available == 0) return;
    unsigned char buffer[1024];
    DWORD count = 0;
    if (!::ReadFile(impl.helper_pipe, buffer,
                    static_cast<DWORD>(sizeof(buffer)), &count, nullptr)) {
      if (::GetLastError() == ERROR_MORE_DATA) continue;
      impl.helper_exited = true;
      if (impl.helper_exit_code < 0) impl.helper_exit_code = 1;
      return;
    }
    if (count == 0) {
      impl.helper_exited = true;
      if (impl.helper_exit_code < 0) impl.helper_exit_code = 1;
      return;
    }
    if (static_cast<terminal_bridge::from_helper>(buffer[0]) ==
        terminal_bridge::from_helper::exit) {
      if (count >= 1 + terminal_bridge::kExitPayload) {
        impl.helper_exit_code =
            static_cast<int>(get_u32(buffer + 1));
      } else {
        impl.helper_exit_code = 0;
      }
      impl.helper_exited = true;
      return;
    }
  }
}

}  // namespace

int pty_read(pty_handle& handle, void* buffer, std::size_t len) {
  if (handle.impl == nullptr) return -1;
  if (handle.impl->mode == pty_impl::kind::helper) {
    return helper_read_frame(*handle.impl, buffer, len);
  }
#if !MOLINK_HAS_CONPTY
  (void)buffer;
  (void)len;
  errno = ENOSYS;
  return -1;
#else
  if (handle.impl->out_read == nullptr) return -1;
  if (len == 0) return 0;
  DWORD available = 0;
  if (!::PeekNamedPipe(handle.impl->out_read, nullptr, 0, nullptr, &available,
                       nullptr)) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_BROKEN_PIPE || code == ERROR_INVALID_HANDLE) return 0;
    errno = EIO;
    return -1;
  }
  if (available == 0) {
    // Same contract as the POSIX master side: nothing right now.
    errno = EAGAIN;
    return -1;
  }
  const DWORD chunk = static_cast<DWORD>(
      (std::min)(len, static_cast<std::size_t>(available)));
  DWORD count = 0;
  if (!::ReadFile(handle.impl->out_read, buffer, chunk, &count, nullptr)) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_BROKEN_PIPE) return 0;
    errno = EIO;
    return -1;
  }
  if (count == 0) return 0;
  return static_cast<int>(count);
#endif
}

bool pty_poll_exit(pty_handle& handle, int& exit_code) {
  if (handle.impl == nullptr) return false;
  if (handle.impl->mode == pty_impl::kind::helper) {
    if (handle.reaped) {
      exit_code = handle.exit_code;
      return handle.pid > 0;
    }
    if (!handle.impl->helper_exited) return false;
    handle.reaped = true;
    handle.exit_code =
        handle.impl->helper_exit_code < 0 ? 1 : handle.impl->helper_exit_code;
    exit_code = handle.exit_code;
    return true;
  }
#if !MOLINK_HAS_CONPTY
  (void)exit_code;
  return false;
#else
  if (handle.reaped) {
    exit_code = handle.exit_code;
    return handle.pid > 0;
  }
  if (handle.impl->process == nullptr) return false;
  if (::WaitForSingleObject(handle.impl->process, 0) != WAIT_OBJECT_0) {
    return false;
  }
  DWORD code = 0;
  if (!::GetExitCodeProcess(handle.impl->process, &code)) {
    code = static_cast<DWORD>(-1);
  }
  handle.reaped = true;
  handle.exit_code = static_cast<int>(code);
  exit_code = handle.exit_code;
  return true;
#endif
}

void pty_kill(pty_handle& handle) {
  if (handle.impl == nullptr) return;
  if (handle.impl->mode == pty_impl::kind::helper) {
    if (!handle.impl->helper_exited && handle.impl->helper_pipe != nullptr) {
      unsigned char frame =
          static_cast<unsigned char>(terminal_bridge::to_helper::close);
      write_pipe_message(handle.impl->helper_pipe, &frame, 1);
      // Give the helper a moment to report the real exit code and to clean
      // the child up before the pipe goes away. Bounded: the event loop calls
      // this from the service poll.
      for (int waited = 0; waited < 500 && !handle.impl->helper_exited;
           waited += 5) {
        helper_drain(*handle.impl);
        if (!handle.impl->helper_exited) ::Sleep(5);
      }
    }
    if (!handle.impl->helper_exited) {
      handle.impl->helper_exited = true;
      if (handle.impl->helper_exit_code < 0) handle.impl->helper_exit_code = -1;
    }
    const int code = handle.impl->helper_exit_code;
    // Closing the pipe makes the helper's input thread see a broken client
    // and terminate the shell, even if the close frame above was lost.
    handle.impl->release();
    handle.reaped = true;
    handle.exit_code = code;
    return;
  }
#if !MOLINK_HAS_CONPTY
  return;
#else
  if (handle.impl->killed) return;
  handle.impl->killed = true;
  if (handle.impl->process != nullptr) {
    if (::WaitForSingleObject(handle.impl->process, 0) == WAIT_TIMEOUT) {
      ::TerminateProcess(handle.impl->process, 1);
      ::WaitForSingleObject(handle.impl->process, 1000);
    }
    int code = -1;
    pty_poll_exit(handle, code);
    ::CloseHandle(handle.impl->process);
    handle.impl->process = nullptr;
  }
  handle.impl->release();
#endif
}

std::string pty_default_shell() {
  const char* shell = std::getenv("COMSPEC");
  if (shell != nullptr && shell[0] != '\0') return shell;
  return "cmd.exe";
}

}  // namespace molink

#endif  // _WIN32
