// Core supervision policy shared by the Windows helper and its unit tests.
//
// The helper owns the molink-core child process: it launches it, waits for it
// to exit, and decides whether to start it again. All of that decision logic
// lives here, free of Win32 calls, so it can be exercised with fake processes
// on any host. `core_supervisor.cpp` supplies the real backend.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace molink {
namespace supervisor {

/// Restart policy. The defaults match the host requirements: 1 s, 2 s, 5 s,
/// then 10 s between restarts, and five restart attempts before giving up.
/// Every abnormal exit counts: the sixth in a row is final.
struct restart_policy {
  std::vector<int64_t> backoff_ms{1000, 2000, 5000, 10000};
  int max_restarts = 5;
  /// How long a graceful core stop may take before the backend force-kills it.
  int64_t stop_grace_ms = 3 * 1000;
  /// Supervisor wait granularity; also bounds how quickly a stop request is
  /// noticed.
  int64_t poll_interval_ms = 500;
};

enum class action {
  /// Do not start the core again (normal exit or an explicit stop request).
  stop,
  /// Start the core again after `delay_ms`.
  restart,
  /// Stop trying; the consecutive failure budget is exhausted.
  give_up,
};

struct state {
  int consecutive_failures = 0;
};

struct decision {
  action act = action::stop;
  int64_t delay_ms = 0;
  /// 1-based restart ordinal, for "attempt 2/5" log lines.
  int attempt = 0;
};

/// Delay before restart number `attempt` (1-based). Values beyond the list
/// reuse the last entry, so 1, 2, 5, 10, 10 for the default policy.
int64_t backoff_for(int attempt, const restart_policy& policy);

/// Decision after a core process has exited.
///
/// * `stop_requested` wins: the helper asked the core to leave.
/// * Exit code 0 is a normal exit and is not restarted. (It covers the user
///   closing the app and the core's own Ctrl+Break shutdown path.)
/// * Otherwise the exit counts as a consecutive failure. The sixth in a row
///   gives up; five restarts are attempted first, matching the field test
///   "kill it six times and it must stop trying".
decision after_exit(int exit_code, bool stop_requested, state& status,
                    const restart_policy& policy);

/// Decision after CreateProcess (or path resolution) failed. The same budget
/// applies, so a missing or unlaunchable core also stops after five retries.
decision after_launch_failure(state& status, const restart_policy& policy);

/// "0x00000001" for an exit code, as written to service.log.
std::string hex_exit_code(int code);

/// The platform side of supervision. Every method is called from the
/// supervisor thread except `stopping()`, which may be polled from it only.
class backend {
 public:
  virtual ~backend() = default;

  /// Acquire the single-supervisor lock. False means another helper owns it.
  virtual bool acquire_instance() = 0;
  virtual void release_instance() = 0;

  /// True when a core this helper did not launch is already running.
  virtual bool external_core_running() = 0;
  /// Wait up to `timeout_ms` for that external core. True once it exited.
  virtual bool wait_external_core(int timeout_ms, int& exit_code) = 0;

  /// Launch the core. Returns false and fills `error` when it cannot start.
  virtual bool start_core(std::string& error, unsigned long& pid) = 0;
  /// Wait up to `timeout_ms` for the launched core. True once it exited.
  virtual bool wait_core(int timeout_ms, int& exit_code) = 0;
  /// Ask the core to leave gracefully, force it after the grace period, and
  /// return its exit code.
  virtual int stop_core() = 0;

  /// The helper must exit (service stop request, helper shutdown, or the
  /// console moved to another session).
  virtual bool stopping() = 0;
  /// The helper was asked to exit without touching the core (the service was
  /// stopped from the app, not switched to another session).
  virtual bool detach_requested() = 0;

  /// Sleep in small steps so a stop request is noticed promptly.
  virtual void sleep_interruptible(int64_t ms) = 0;

  virtual void log_service(const std::string& line) = 0;
  /// Structured diagnostic line, reusing the core's `app.exit` convention.
  virtual void log_app_exit(int code, const std::string& reason) = 0;
};

/// Run the supervision loop until the core exits normally, the restart budget
/// is exhausted, or the backend reports `stopping()`.
void run(backend& backend, const restart_policy& policy);

}  // namespace supervisor
}  // namespace molink
