#include "terminal/pty.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

std::string win_error(const char* call, DWORD code) {
  return std::string(call) + " failed (win32 error " + std::to_string(code) +
         ")";
}
#endif  // MOLINK_HAS_CONPTY

}  // namespace

#if MOLINK_HAS_CONPTY

/// One ConPTY session. Shared by every copy of a pty_handle so a copied handle
/// cannot double-close the same pipes or leak them when the last copy dies.
struct pty_impl {
  HPCON pc = nullptr;
  HANDLE in_write = nullptr;  // viewer -> child (the child's stdin)
  HANDLE out_read = nullptr;  // child -> viewer (the child's stdout/stderr)
  HANDLE process = nullptr;
  bool killed = false;

  ~pty_impl() { release(); }

  void release() {
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
  }
};

#endif  // MOLINK_HAS_CONPTY

bool pty_supported() {
#if MOLINK_HAS_CONPTY
  return true;
#else
  return false;
#endif
}

bool pty_valid(const pty_handle& handle) {
#if MOLINK_HAS_CONPTY
  return handle.impl != nullptr && handle.impl->pc != nullptr;
#else
  (void)handle;
  return false;
#endif
}

pty_handle pty_spawn(const std::string& command, int cols, int rows,
                     std::string& err) {
  pty_handle handle;
  err.clear();
  cols = clamp_dimension(cols, 80);
  rows = clamp_dimension(rows, 24);
#if !MOLINK_HAS_CONPTY
  (void)command;
  err =
      "terminal is not supported on this Windows version (ConPTY requires "
      "Windows 10 1809 or newer)";
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
  return handle;
#endif
}

bool pty_resize(pty_handle& handle, int cols, int rows) {
#if !MOLINK_HAS_CONPTY
  (void)handle;
  (void)cols;
  (void)rows;
  return false;
#else
  if (handle.impl == nullptr || handle.impl->pc == nullptr) return false;
  cols = clamp_dimension(cols, 80);
  rows = clamp_dimension(rows, 24);
  const COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
  return SUCCEEDED(::ResizePseudoConsole(handle.impl->pc, size));
#endif
}

int pty_write(pty_handle& handle, const void* data, std::size_t len) {
#if !MOLINK_HAS_CONPTY
  (void)handle;
  (void)data;
  (void)len;
  return -1;
#else
  if (handle.impl == nullptr || handle.impl->in_write == nullptr) return -1;
  const char* bytes = static_cast<const char*>(data);
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

int pty_read(pty_handle& handle, void* buffer, std::size_t len) {
#if !MOLINK_HAS_CONPTY
  (void)handle;
  (void)buffer;
  (void)len;
  errno = ENOSYS;
  return -1;
#else
  if (handle.impl == nullptr || handle.impl->out_read == nullptr) return -1;
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
#if !MOLINK_HAS_CONPTY
  (void)handle;
  (void)exit_code;
  return false;
#else
  if (handle.impl == nullptr) return false;
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
#if !MOLINK_HAS_CONPTY
  (void)handle;
#else
  if (handle.impl == nullptr || handle.impl->killed) return;
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
