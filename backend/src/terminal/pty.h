#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace molink {

/// Platform-private backend state (the ConPTY handles on Windows). Defined in
/// pty.cpp so <windows.h> — and its min()/max() macros — never reaches this
/// header's other includers.
struct pty_impl;

/// A spawned pseudo-terminal. `fd` is the master side; `pid` is the child
/// process that owns the slave side. On Windows `fd` stays -1 and the ConPTY
/// handles live in `impl`; use pty_valid() instead of inspecting `fd`.
///
/// The handle is copyable (terminal_session stores one by value). On Windows
/// the copies share `impl`, whose destructor releases the handles, so a stray
/// copy cannot leak pipes or a pseudoconsole.
struct pty_handle {
  int fd = -1;
  int pid = -1;
  /// Set once waitpid() has collected the child; exit_code is valid then.
  bool reaped = false;
  int exit_code = -1;
  std::shared_ptr<pty_impl> impl;
};

/// Whether this handle refers to a live PTY (spawn succeeded, not killed yet).
bool pty_valid(const pty_handle& handle);

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
