// The host-side frame clock: cadence, the viewer feedback that lowers it, and
// the recovery back to the capture rate.
#include <cstdio>

#include "capture/video_push.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void expect(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

void test_cadence() {
  molink::video_push_clock clock(30);
  expect(clock.interval_ms() == 33, "30 fps is one frame per 33 ms");
  expect(clock.due(1000), "the first tick is always due");
  expect(!clock.due(1010), "a tick inside the interval is not due");
  expect(!clock.due(1032), "one millisecond early is still not due");
  expect(clock.due(1033), "the interval boundary is due");
  expect(clock.due(1100), "an overdue tick fires instead of piling up");
  expect(!clock.due(1110), "the cadence restarts from the frame actually sent");
}

void test_target_changes() {
  molink::video_push_clock clock(60);
  expect(clock.interval_ms() == 16, "60 fps is one frame per 16 ms");
  clock.set_target_fps(10);
  expect(clock.fps() == 10 && clock.interval_ms() == 100,
         "the capture rate sets the clock");
  clock.set_target_fps(1000);
  expect(clock.fps() == 120, "the clock is clamped to its ceiling");
  clock.set_target_fps(0);
  expect(clock.fps() == 5, "the clock is clamped to its floor");
}

void test_viewer_backpressure() {
  molink::video_push_clock clock(60);
  clock.due(0);
  // A viewer painting a third of what it is sent, with drops, halves the
  // clock -- once per second at most.
  clock.note_viewer(20.0, 5, 1000);
  expect(clock.fps() == 30, "a slow viewer halves the clock");
  clock.note_viewer(5.0, 5, 1200);
  expect(clock.fps() == 30, "feedback inside the same second is ignored");
  clock.note_viewer(10.0, 2, 2100);
  expect(clock.fps() == 15, "a second slow sample halves it again");
  clock.note_viewer(1.0, 1, 3200);
  clock.note_viewer(1.0, 1, 4300);
  clock.note_viewer(1.0, 1, 5400);
  clock.note_viewer(1.0, 1, 6500);
  expect(clock.fps() == molink::video_push_clock::kMinFps,
         "the clock never drops below its floor");
}

void test_viewer_recovery() {
  molink::video_push_clock clock(60);
  clock.due(0);
  clock.note_viewer(10.0, 3, 1000);
  clock.note_viewer(5.0, 3, 2000);
  expect(clock.fps() == 15, "two slow samples leave the clock at a quarter");
  clock.note_viewer(14.0, 0, 3000);
  expect(clock.fps() == 30, "a viewer that keeps up recovers one step");
  clock.note_viewer(28.0, 0, 4000);
  expect(clock.fps() == 45, "and again on the next good second");
  clock.note_viewer(44.0, 0, 5000);
  expect(clock.fps() == 60, "recovery stops at the capture rate");
  clock.note_viewer(60.0, 0, 6000);
  expect(clock.fps() == 60, "the ceiling holds");
}

void test_drops_only_without_paint_loss() {
  molink::video_push_clock clock(30);
  clock.due(0);
  // Dropped frames with a healthy paint rate are not a reason to slow down:
  // the viewer is skipping what it never needed (static screen, duplicate).
  clock.note_viewer(29.0, 4, 1000);
  expect(clock.fps() == 30, "drops alone do not lower the clock");
  clock.note_viewer(0.0, 4, 2000);
  expect(clock.fps() == 30, "a silent viewer is ignored");
}

}  // namespace

int main() {
  test_cadence();
  test_target_changes();
  test_viewer_backpressure();
  test_viewer_recovery();
  test_drops_only_without_paint_loss();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
