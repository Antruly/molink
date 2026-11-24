// Absolute-deadline capture pacing: the period is the target interval, not
// the interval plus the encode time, and a late grab never accumulates.
#include <cstdio>

#include "capture/frame_schedule.h"

namespace {

int checks = 0;
int failures = 0;

void expect(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

constexpr int64_t kInterval = 100000;

void test_grid_does_not_accumulate() {
  molink::frame_schedule schedule;
  schedule.reset(0, kInterval);
  expect(schedule.next() == kInterval, "the first frame is one interval out");
  expect(!schedule.due(kInterval - 1), "not due before the deadline");
  expect(schedule.due(kInterval), "due at the deadline");

  // A 60 ms encode: the capture completes 60 ms after its deadline, but the
  // next deadline still lands on the 100 ms grid.
  schedule.note_capture(160000);
  expect(schedule.next() == 2 * kInterval,
         "the next deadline stays on the absolute grid");
  schedule.note_capture(260000);
  expect(schedule.next() == 3 * kInterval,
         "the schedule does not drift by the encode time");

  // Fifty frames at the target rate must still occupy exactly five seconds.
  molink::frame_schedule long_run;
  long_run.reset(0, kInterval);
  for (int i = 1; i <= 50; ++i) {
    long_run.note_capture(static_cast<int64_t>(i) * kInterval + 60000);
  }
  expect(long_run.next() == 51 * kInterval,
         "the deadline after fifty slow frames is still on the grid");
}

void test_late_grab_restarts_instead_of_replaying() {
  molink::frame_schedule schedule;
  schedule.reset(0, kInterval);
  // The grab overran by 2.5 intervals. The missed slots must not be queued.
  schedule.note_capture(350000);
  expect(schedule.next() == 350000,
         "a late grab makes the next capture due immediately");
  expect(schedule.restarted(), "the catch-up is reported");
  expect(schedule.due(350000), "the immediate capture is due");
  schedule.clear_restarted();

  // That immediate capture completes right away; the grid is fresh.
  schedule.note_capture(360000);
  expect(!schedule.restarted(), "the catch-up is one frame, not a burst");
  expect(schedule.next() == 450000,
         "the schedule restarts one interval after the catch-up frame");
}

void test_early_input_reanchors() {
  molink::frame_schedule schedule;
  schedule.reset(0, kInterval);
  // Input arrived at 30 ms and the caller skipped the wait.
  schedule.note_capture(40000, /*reanchor=*/true);
  expect(schedule.next() == 140000,
         "an early input frame owns a fresh deadline instead of pushing the "
         "next one out by a whole interval");
}

void test_interval_change_reanchors() {
  molink::frame_schedule schedule;
  schedule.reset(0, kInterval);
  schedule.set_interval(50000, 50000);
  expect(schedule.next() == 100000 && schedule.interval() == 50000,
         "a new target rate re-anchors the grid");
  expect(schedule.due(100000), "the new deadline is honoured");
}

}  // namespace

int main() {
  test_grid_does_not_accumulate();
  test_late_grab_restarts_instead_of_replaying();
  test_early_input_reanchors();
  test_interval_change_reanchors();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
