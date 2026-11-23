#include "supervisor_policy.h"

#include <cstdio>

namespace molink {
namespace supervisor {

int64_t backoff_for(int attempt, const restart_policy& policy) {
  if (policy.backoff_ms.empty()) return 1000;
  if (attempt <= 1) return policy.backoff_ms.front();
  const std::size_t index = static_cast<std::size_t>(attempt - 1);
  if (index >= policy.backoff_ms.size()) return policy.backoff_ms.back();
  return policy.backoff_ms[index];
}

decision after_exit(int exit_code, bool stop_requested, state& status,
                    const restart_policy& policy) {
  decision result;
  if (stop_requested) {
    status.consecutive_failures = 0;
    return result;
  }
  if (exit_code == 0) {
    // Normal exit: the user closed the session, or the core shut itself down.
    status.consecutive_failures = 0;
    return result;
  }

  ++status.consecutive_failures;
  if (status.consecutive_failures > policy.max_restarts) {
    result.act = action::give_up;
    return result;
  }
  result.act = action::restart;
  result.attempt = status.consecutive_failures;
  result.delay_ms = backoff_for(result.attempt, policy);
  return result;
}

decision after_launch_failure(state& status, const restart_policy& policy) {
  decision result;
  ++status.consecutive_failures;
  if (status.consecutive_failures > policy.max_restarts) {
    result.act = action::give_up;
    return result;
  }
  result.act = action::restart;
  result.attempt = status.consecutive_failures;
  result.delay_ms = backoff_for(result.attempt, policy);
  return result;
}

std::string hex_exit_code(int code) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08X",
                static_cast<unsigned int>(code));
  return std::string(buffer);
}

namespace {

std::string attempt_text(const decision& decided, const restart_policy& policy) {
  return std::to_string(decided.attempt) + "/" +
         std::to_string(policy.max_restarts);
}

void log_restart(backend& side, const decision& decided,
                 const restart_policy& policy) {
  side.log_service("core restart in " + std::to_string(decided.delay_ms) +
                   "ms (attempt " + attempt_text(decided, policy) + ")");
}

/// Wait for a core this helper did not launch. Returns true when the caller
/// should return from run(): a normal exit, an exhausted restart budget, or a
/// stop request. Returns false when the core left abnormally and the caller
/// should launch one after the backoff.
bool wait_for_external(backend& side, const restart_policy& policy,
                       state& status) {
  side.log_service("core already running, waiting for it");
  for (;;) {
    if (side.stopping()) {
      if (side.detach_requested()) {
        side.log_service("helper detached; core left running");
      } else {
        const int code = side.stop_core();
        side.log_app_exit(code, "stop");
        side.log_service("core stopped (helper shutdown)");
      }
      return true;
    }
    int code = 0;
    if (!side.wait_external_core(static_cast<int>(policy.poll_interval_ms),
                                 code)) {
      continue;
    }
    side.log_service("core exited code=" + hex_exit_code(code));
    const bool normal = code == 0;
    side.log_app_exit(code, normal ? "normal" : "crash");
    const decision decided = after_exit(code, false, status, policy);
    if (decided.act == action::stop) {
      side.log_service("core exited normally, not restarting");
      return true;
    }
    if (decided.act == action::give_up) {
      side.log_service("core restart gave up");
      return true;
    }
    log_restart(side, decided, policy);
    side.sleep_interruptible(decided.delay_ms);
    return false;
  }
}

}  // namespace

void run(backend& side, const restart_policy& policy) {
  if (!side.acquire_instance()) {
    side.log_service("core supervisor already running");
    return;
  }
  struct instance_guard {
    backend& side;
    ~instance_guard() { side.release_instance(); }
  } guard{side};

  state status;

  // A core started by the desktop shell (or a previous helper that could not
  // kill it) must not be duplicated: both would fight over port 28418.
  if (side.external_core_running()) {
    if (wait_for_external(side, policy, status)) return;
  }

  for (;;) {
    if (side.stopping()) {
      if (!side.detach_requested()) side.stop_core();
      return;
    }

    // A core may have appeared while we were backing off (the desktop shell
    // restarted it, or a previous helper's core was not reaped yet). Adopt it
    // instead of launching a second listener.
    if (side.external_core_running()) {
      if (wait_for_external(side, policy, status)) return;
      continue;
    }

    std::string error;
    unsigned long pid = 0;
    if (!side.start_core(error, pid)) {
      side.log_service("core start failed: " + error);
      const decision decided = after_launch_failure(status, policy);
      if (decided.act == action::stop) return;
      if (decided.act == action::give_up) {
        side.log_service("core restart gave up");
        return;
      }
      log_restart(side, decided, policy);
      side.sleep_interruptible(decided.delay_ms);
      continue;
    }

    side.log_service("core started pid=" + std::to_string(pid));

    for (;;) {
      if (side.stopping()) {
        if (side.detach_requested()) {
          side.log_service("helper detached; core left running");
        } else {
          const int code = side.stop_core();
          side.log_app_exit(code, "stop");
          side.log_service("core stopped (helper shutdown)");
        }
        return;
      }
      int code = 0;
      if (side.wait_core(static_cast<int>(policy.poll_interval_ms), code)) {
        if (side.stopping()) {
          if (side.detach_requested()) {
            side.log_service("helper detached; core left running");
          } else {
            side.stop_core();
          }
          return;
        }
        side.log_service("core exited code=" + hex_exit_code(code));
        const bool normal = code == 0;
        side.log_app_exit(code, normal ? "normal" : "crash");

        const decision decided = after_exit(code, false, status, policy);
        if (decided.act == action::stop) {
          side.log_service("core exited normally, not restarting");
          return;
        }
        if (decided.act == action::give_up) {
          side.log_service("core restart gave up");
          return;
        }
        log_restart(side, decided, policy);
        side.sleep_interruptible(decided.delay_ms);
        break;
      }
    }
  }
}

}  // namespace supervisor
}  // namespace molink
