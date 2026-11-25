#include "terminal/pty.h"

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

namespace molink {

bool pty_supported() { return false; }

pty_handle pty_spawn(const std::string&, int, int, std::string& err) {
  err = "terminal is not supported on this platform yet (ConPTY pending)";
  return pty_handle{};
}

bool pty_resize(pty_handle&, int, int) { return false; }

int pty_write(pty_handle&, const void*, std::size_t) { return -1; }

int pty_read(pty_handle&, void*, std::size_t) { return -1; }

bool pty_poll_exit(pty_handle&, int&) { return false; }

void pty_kill(pty_handle&) {}

std::string pty_default_shell() {
  const char* shell = std::getenv("COMSPEC");
  if (shell != nullptr && shell[0] != '\0') return shell;
  return "cmd.exe";
}

}  // namespace molink

#endif  // _WIN32
