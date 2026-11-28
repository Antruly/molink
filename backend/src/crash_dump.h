#pragma once

#include <string>

namespace molink {

/// Install handlers that leave a crash report behind for a fatal crash.
///
/// POSIX: SIGSEGV, SIGABRT, SIGBUS, SIGILL (and SIGFPE) write
/// `<config_dir>/crashdumps/crash-<pid>-<unix_ts>.txt` with the signal, the
/// fault address, the process start time, the version and the raw backtrace
/// frames, then `_exit(128+signal)`.
///
/// Windows: an unhandled exception writes
/// `<config_dir>/crashdumps/core-<pid>-<ts>.dmp` (MiniDumpNormal |
/// MiniDumpWithHandleData) plus a `.txt` with the exception code and address.
///
/// The handler only uses async-signal-safe calls (open/write/close/_exit and
/// backtrace_symbols_fd); the directory and every other string it needs are
/// prepared here, while normal allocation and logging are still allowed.
///
/// @p diag_file, when non-empty, also receives one
/// `app.crash signal=.. addr=.. dump=..` line. It is opened from the handler
/// with a plain append write, never through diag(), which uses mutexes.
void install_crash_handlers(const std::string& config_dir,
                            const std::string& diag_file);

/// Point the handler's `app.crash` line at today's diagnostic file. Called at
/// startup and once per day by the maintenance thread; an empty path disables
/// that line. Safe to call while other threads run.
void set_crash_diag_file(const std::string& path);

}  // namespace molink
