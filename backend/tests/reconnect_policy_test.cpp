// The reconnect schedule must match the Web viewer: one immediate attempt,
// two one-second attempts, then one per minute.
#include <cstdint>
#include <cstdio>

#include "reconnect_policy.h"

namespace {

int failures = 0;
int checks = 0;

using molink::native::reconnect::delay_for_attempt;
using molink::native::reconnect::is_quick_attempt;
using molink::native::reconnect::seconds_remaining;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void test_delay_sequence() {
  expect(delay_for_attempt(1) == 0, "the first retry is immediate");
  expect(delay_for_attempt(2) == 1000, "the second retry waits one second");
  expect(delay_for_attempt(3) == 1000, "the third retry waits one second");
  expect(delay_for_attempt(4) == 60000, "the fourth retry waits a minute");
  expect(delay_for_attempt(5) == 60000, "later retries stay at one minute");
  expect(delay_for_attempt(99) == 60000, "the slow interval is stable");
  expect(is_quick_attempt(1) && is_quick_attempt(2) &&
             is_quick_attempt(3),
         "the first three attempts are quick");
  expect(!is_quick_attempt(4), "the fourth attempt is the slow retry");
  expect(!is_quick_attempt(0), "attempt zero is not a retry");
}

void test_countdown() {
  expect(seconds_remaining(60000, 0) == 60, "a full minute counts as 60");
  expect(seconds_remaining(60000, 59000) == 1, "the last second counts as 1");
  expect(seconds_remaining(60000, 60000) == 0, "a due retry counts as 0");
  expect(seconds_remaining(60000, 61000) == 0,
         "an overdue retry never counts backwards");
  expect(seconds_remaining(1500, 0) == 2, "partial seconds round up");
}

}  // namespace

int main() {
  test_delay_sequence();
  test_countdown();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
