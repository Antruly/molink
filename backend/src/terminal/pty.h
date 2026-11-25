#pragma once

#include <cstddef>
#include <string>

namespace molink {

/// A spawned pseudo-terminal. `fd` is the master side; `pid` is the child
/// process that owns the slave side. On Windows the backend is not there yet
/// (ConPTY is the next batch), so pty_spawn() reports an explicit error and
/// pty_valid() stays false.
struct pty_handle {
  int fd = -1;
  int pid = -1;
  /// Set once waitpid() has collected the child; exit_code is valid then.
  bool reaped = false;
  int exit_code = -1;
};

/// Whether this build has a usable PTY backend.
bool pty_supported();

/// Spawn `command` on a new pseudo-terminal of `cols` x `rows`.
///
/// `command` is a command line, not necessarily just an executable: it is
/// split on whitespace with single/double quote support, and argv[0] is looked
/// up on PATH. An empty command uses $SHELL (or /bin/sh). The child inherits
/// the current environment unchanged; no login shell or re-authentication is
/// involved.
///
/// On failure returns a default (invalid) handle and fills `err`.
pty_handle pty_spawn(const std::string& command, int cols, int rows,
                     std::string& err);

/// Apply a new window size (TIOCSWINSZ) and tell the child's process group
/// with SIGWINCH.
bool pty_resize(pty_handle& handle, int cols, int rows);

/// Write bytes to the terminal. Non-blocking: -1 with errno == EAGAIN when
/// the kernel buffer is full.
int pty_write(pty_handle& handle, const void* data, std::size_t len);

/// Read bytes from the terminal. Non-blocking: -1 with errno == EAGAIN when
/// nothing is available, 0 at end of file, -1 with EIO after the child exits
/// and the slave side is closed.
int pty_read(pty_handle& handle, void* buffer, std::size_t len);

/// Reap the child if it has exited. Returns true once, with `exit_code` set.
/// A signal death is reported as 128 + the signal number.
bool pty_poll_exit(pty_handle& handle, int& exit_code);

/// SIGHUP the child's process group, then SIGKILL it, reap the child and close
/// the master side. Safe to call more than once, including after the child was
/// already reaped.
void pty_kill(pty_handle& handle);

/// $SHELL, or /bin/sh when it is unset or empty.
std::string pty_default_shell();

}  // namespace molink
