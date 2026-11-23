// Unit tests for the portable core supervision policy.
//
// No Windows process is involved: a fake backend scripts core exits and the
// instance lock, so the restart/backoff/give-up decisions run everywhere.
#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "supervisor_policy.h"

namespace {

using namespace molink::supervisor;

int g_failures = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  ++g_failures;
  std::fprintf(stderr, "FAIL: %s\n", what);
}

bool contains(const std::vector<std::string>& lines, const std::string& text) {
  for (const std::string& line : lines) {
    if (line.find(text) != std::string::npos) return true;
  }
  return false;
}

bool has_app_exit(const std::vector<std::pair<int, std::string>>& exits, int code,
                  const std::string& reason) {
  for (const auto& entry : exits) {
    if (entry.first == code && entry.second == reason) return true;
  }
  return false;
}

class fake_backend : public backend {
 public:
  bool lock_ok = true;
  bool stopping_now = false;
  bool external = false;
  int external_code = 1;
  int start_failures = 0;
  std::vector<int> exit_codes;
  bool detach = false;
  bool detach_on_wait = false;
  bool external_after_first_sleep = false;

  int start_calls = 0;
  int release_calls = 0;
  int stop_calls = 0;
  int external_waits = 0;
  std::size_t next_exit = 0;
  std::vector<int64_t> sleeps;
  std::vector<std::string> service_log;
  std::vector<std::pair<int, std::string>> app_exits;

  bool acquire_instance() override { return lock_ok; }
  void release_instance() override { ++release_calls; }

  bool external_core_running() override { return external; }
  bool wait_external_core(int, int& exit_code) override {
    ++external_waits;
    if (!external) return false;
    external = false;
    exit_code = external_code;
    return true;
  }

  bool start_core(std::string& error, unsigned long& pid) override {
    ++start_calls;
    if (start_failures > 0) {
      --start_failures;
      error = "cannot start";
      return false;
    }
    pid = static_cast<unsigned long>(100 + start_calls);
    return true;
  }

  bool wait_core(int, int& exit_code) override {
    if (next_exit < exit_codes.size()) {
      exit_code = exit_codes[next_exit++];
      return true;
    }
    // No more scripted exits: end the test by asking the helper to stop.
    stopping_now = true;
    if (detach_on_wait) detach = true;
    return false;
  }

  int stop_core() override {
    ++stop_calls;
    return 0;
  }

  bool stopping() override { return stopping_now; }
  bool detach_requested() override { return detach; }
  void sleep_interruptible(int64_t ms) override {
    sleeps.push_back(ms);
    if (external_after_first_sleep) {
      external = true;
      external_code = 0;
      external_after_first_sleep = false;
    }
  }

  void log_service(const std::string& line) override {
    service_log.push_back(line);
  }
  void log_app_exit(int code, const std::string& reason) override {
    app_exits.emplace_back(code, reason);
  }
};

void test_backoff() {
  restart_policy policy;
  expect(backoff_for(1, policy) == 1000, "attempt 1 backs off 1 s");
  expect(backoff_for(2, policy) == 2000, "attempt 2 backs off 2 s");
  expect(backoff_for(3, policy) == 5000, "attempt 3 backs off 5 s");
  expect(backoff_for(4, policy) == 10000, "attempt 4 backs off 10 s");
  expect(backoff_for(5, policy) == 10000, "attempt 5 reuses 10 s");
  expect(backoff_for(99, policy) == 10000, "large attempts clamp to 10 s");

  restart_policy empty;
  empty.backoff_ms.clear();
  expect(backoff_for(1, empty) == 1000, "empty backoff list falls back to 1 s");
}

void test_pure_decisions() {
  restart_policy policy;
  state status;

  decision normal = after_exit(0, false, status, policy);
  expect(normal.act == action::stop, "exit code 0 is a normal stop");
  expect(status.consecutive_failures == 0, "normal exit clears failures");

  decision requested = after_exit(1, true, status, policy);
  expect(requested.act == action::stop, "a requested stop is not restarted");

  decision first = after_exit(1, false, status, policy);
  expect(first.act == action::restart, "abnormal exit restarts");
  expect(first.delay_ms == 1000, "first restart waits 1 s");
  expect(first.attempt == 1, "first restart is attempt 1");
  expect(status.consecutive_failures == 1, "first crash counted");

  state run_away;
  int give_up_at = 0;
  for (int i = 1; i <= 10; ++i) {
    const decision decided = after_exit(1, false, run_away, policy);
    if (decided.act == action::give_up) {
      give_up_at = i;
      break;
    }
  }
  expect(give_up_at == 6, "the sixth consecutive crash gives up");

  state launch;
  int launch_give_up = 0;
  for (int i = 1; i <= 10; ++i) {
    const decision decided = after_launch_failure(launch, policy);
    if (decided.act == action::give_up) {
      launch_give_up = i;
      break;
    }
  }
  expect(launch_give_up == 6, "the sixth consecutive launch failure gives up");

  expect(hex_exit_code(1) == "0x00000001", "exit code is formatted in hex");
  expect(hex_exit_code(0xC0000005) == "0xC0000005",
         "exception exit codes keep their bits");
}

void test_normal_exit_is_not_restarted() {
  fake_backend side;
  side.exit_codes = {0};
  restart_policy policy;
  run(side, policy);
  expect(side.start_calls == 1, "a normal exit starts the core once");
  expect(side.sleeps.empty(), "a normal exit does not back off");
  expect(contains(side.service_log, "core exited normally, not restarting"),
         "a normal exit is logged");
  expect(side.app_exits.size() == 1 && side.app_exits[0].first == 0 &&
             side.app_exits[0].second == "normal",
         "app.exit records the normal exit");
  expect(side.release_calls == 1, "the instance lock is released");
}

void test_abnormal_exit_restarts() {
  fake_backend side;
  side.exit_codes = {1};
  restart_policy policy;
  run(side, policy);
  expect(side.start_calls == 2, "an abnormal exit restarts the core");
  expect(side.sleeps.size() == 1 && side.sleeps[0] == 1000,
         "first restart waits 1 s");
  expect(contains(side.service_log, "core exited code=0x00000001"),
         "service.log records the exit code");
  expect(contains(side.service_log, "core restart in 1000ms (attempt 1/5)"),
         "service.log records the restart delay");
  expect(side.app_exits.size() >= 1 && has_app_exit(side.app_exits, 1, "crash"),
         "app.exit records the crash");
}

void test_gives_up_after_five_restarts() {
  fake_backend side;
  side.exit_codes = {1, 1, 1, 1, 1, 1};
  restart_policy policy;
  run(side, policy);
  expect(side.start_calls == 6, "one launch plus five restarts");
  expect(side.sleeps.size() == 5, "five backoff waits");
  if (side.sleeps.size() == 5) {
    expect(side.sleeps[0] == 1000, "restart 1 waits 1 s");
    expect(side.sleeps[1] == 2000, "restart 2 waits 2 s");
    expect(side.sleeps[2] == 5000, "restart 3 waits 5 s");
    expect(side.sleeps[3] == 10000, "restart 4 waits 10 s");
    expect(side.sleeps[4] == 10000, "restart 5 waits 10 s");
  }
  expect(contains(side.service_log, "core restart gave up"),
         "service.log records the give-up");
  expect(side.start_calls == 6, "the sixth kill has no following launch");
}

void test_launch_failures_give_up() {
  fake_backend side;
  side.start_failures = 6;
  restart_policy policy;
  run(side, policy);
  expect(side.start_calls == 6, "six launch attempts");
  expect(side.sleeps.size() == 5, "five waits between launch attempts");
  expect(contains(side.service_log, "core start failed: cannot start"),
         "the launch error is logged");
  expect(contains(side.service_log, "core restart gave up"),
         "launch failures also give up");
}

void test_single_instance() {
  fake_backend side;
  side.lock_ok = false;
  restart_policy policy;
  run(side, policy);
  expect(side.start_calls == 0, "a held instance lock never starts a core");
  expect(contains(side.service_log, "core supervisor already running"),
         "the held lock is logged");
  expect(side.release_calls == 0, "a lock we did not acquire is not released");
}

void test_existing_core_is_not_duplicated() {
  fake_backend side;
  side.external = true;
  side.external_code = 1;
  side.exit_codes = {0};
  restart_policy policy;
  run(side, policy);
  expect(side.external_waits >= 1, "the existing core is watched");
  expect(side.start_calls == 1, "a new core starts only after the old one exits");
  expect(side.sleeps.size() == 1 && side.sleeps[0] == 1000,
         "the adopted core's crash follows the normal backoff");
  expect(contains(side.service_log, "core already running, waiting for it"),
         "adoption is logged");
}

void test_existing_core_normal_exit_does_not_restart() {
  fake_backend side;
  side.external = true;
  side.external_code = 0;
  restart_policy policy;
  run(side, policy);
  expect(side.start_calls == 0, "a normal exit of the existing core is final");
  expect(contains(side.service_log, "core exited normally, not restarting"),
         "the normal adoption exit is logged");
}

void test_six_kills_give_up_even_when_the_first_core_was_adopted() {
  fake_backend side;
  side.external = true;       // the first core is the desktop shell's
  side.external_code = 1;     // kill #1
  side.exit_codes = {1, 1, 1, 1, 1};  // kills #2..#6 on helper-launched cores
  restart_policy policy;
  run(side, policy);
  expect(side.external_waits >= 1, "the adopted core is watched first");
  expect(side.start_calls == 5, "five restarts follow the adopted crash");
  expect(side.sleeps.size() == 5, "all six crashes share one failure budget");
  expect(contains(side.service_log, "core restart gave up"),
         "the sixth kill gives up even when the first core was adopted");
}

void test_stop_request_during_run() {
  fake_backend side;
  side.exit_codes = {};
  restart_policy policy;
  run(side, policy);
  expect(side.start_calls == 1, "the core starts before the stop request");
  expect(side.stop_calls == 1, "a stop request stops the running core");
  expect(side.sleeps.empty(), "a stop request never backs off");
  expect(contains(side.service_log, "core stopped (helper shutdown)"),
         "the helper shutdown is logged");
}

void test_detach_leaves_the_core_running() {
  fake_backend side;
  side.exit_codes = {};
  side.detach_on_wait = true;
  restart_policy policy;
  run(side, policy);
  expect(side.start_calls == 1, "the core starts before the detach request");
  expect(side.stop_calls == 0, "a detach request must not stop the core");
  expect(contains(side.service_log, "helper detached; core left running"),
         "the detach is logged");
  expect(!contains(side.service_log, "core stopped (helper shutdown)"),
         "a detach is not a core shutdown");
}

void test_core_appearing_during_backoff_is_adopted() {
  fake_backend side;
  side.exit_codes = {1};
  side.external_after_first_sleep = true;
  restart_policy policy;
  run(side, policy);
  expect(side.start_calls == 1, "the helper launched the first core");
  expect(side.external_waits >= 1,
         "a core that appears during the backoff is adopted");
  expect(contains(side.service_log, "core exited normally, not restarting"),
         "the adopted core's normal exit is final");
}

}  // namespace

int main() {
  test_backoff();
  test_pure_decisions();
  test_normal_exit_is_not_restarted();
  test_abnormal_exit_restarts();
  test_gives_up_after_five_restarts();
  test_launch_failures_give_up();
  test_single_instance();
  test_existing_core_is_not_duplicated();
  test_existing_core_normal_exit_does_not_restart();
  test_six_kills_give_up_even_when_the_first_core_was_adopted();
  test_stop_request_during_run();
  test_detach_leaves_the_core_running();
  test_core_appearing_during_backoff_is_adopted();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d supervisor policy check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("supervisor policy checks passed\n");
  return 0;
}
