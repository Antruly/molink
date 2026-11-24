// Weight-based stream QoS: the controller must squeeze the budget only on
// sustained pressure, dwell between moves, and climb back proportionally.
#include <cmath>
#include <cstdio>
#include <string>

#include "capture/stream_qos.h"

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

bool close_to(double value, double expected) {
  return std::fabs(value - expected) < 1e-9;
}

molink::stream_qos::input healthy() {
  molink::stream_qos::input in;
  in.paint_fps = 30;
  in.target_fps = 30;
  in.dropped = 0;
  in.rtt_ms = 20;
  in.queue_delay_ms = 20;
  in.demand = true;
  return in;
}

/// A viewer stall reported through the drop fallback (no queue measurement).
molink::stream_qos::input stalled() {
  molink::stream_qos::input in;
  in.paint_fps = 4;
  in.target_fps = 30;
  in.dropped = 3;
  in.rtt_ms = 400;
  in.demand = true;
  return in;
}

void test_healthy_is_quiet() {
  molink::stream_qos qos;
  const auto in = healthy();
  for (int i = 0; i < 10; ++i) {
    expect(!qos.note(in, static_cast<uint64_t>(i) * 1000),
           "a healthy window changes nothing");
  }
  expect(qos.bitrate_scale() == 1.0, "the budget anchor stays untouched");
  expect(!qos.force_cbr(), "no hard ceiling while healthy");
  expect(qos.last_change() == molink::stream_qos::change::none,
         "a healthy run writes no quality.qos move");
}

void test_stall_needs_two_windows_and_dwells() {
  molink::stream_qos qos;
  const auto in = stalled();
  expect(!qos.note(in, 0), "one bad window is not yet a trend");
  expect(qos.bitrate_scale() == 1.0, "the first bad window leaves the scale");

  expect(qos.note(in, 1000), "the second consecutive bad window moves");
  expect(close_to(qos.bitrate_scale(), molink::stream_qos::kDownStep),
         "the down step is 0.85, not a 0.75 overshoot");
  expect(qos.last_change() == molink::stream_qos::change::down,
         "the move is reported as a down");
  expect(std::string(qos.last_reason()) == "dropped",
         "the reason names the criterion that triggered it");

  // Still bad, but the three-second dwell blocks the next move.
  expect(!qos.note(in, 2000), "the dwell holds the second step back");
  expect(!qos.note(in, 3000), "still inside the dwell");
  expect(qos.note(in, 4000), "the next step is due three seconds later");
  expect(close_to(qos.bitrate_scale(),
                  molink::stream_qos::kDownStep * molink::stream_qos::kDownStep),
         "the scale steps down monotonically");
  expect(qos.last_dwell_ms() == 3000, "the reported dwell is three seconds");
}

void test_dropped_fallback_requires_consecutive_windows() {
  molink::stream_qos qos;
  auto in = healthy();
  in.queue_delay_ms = 0;  // no queueing measurement
  in.dropped = 1;
  expect(!qos.note(in, 0), "a single window with a drop does not move");
  in.dropped = 0;
  expect(!qos.note(in, 1000), "a clean window breaks the streak");
  expect(qos.bitrate_scale() == 1.0, "no downgrade from an isolated drop");

  molink::stream_qos persistent;
  in.dropped = 1;
  expect(!persistent.note(in, 0), "first dropped window is ignored");
  expect(persistent.note(in, 1000),
         "two consecutive dropped windows move the scale");
}

void test_queue_delay_is_the_preferred_signal() {
  molink::stream_qos qos;
  auto in = healthy();
  in.queue_delay_ms = molink::stream_qos::kQueueDelayBadMs + 20;
  in.dropped = 0;
  expect(!qos.note(in, 0), "queue delay also needs a second window");
  expect(qos.note(in, 1000), "sustained queueing delay squeezes the budget");
  expect(std::string(qos.last_reason()) == "queue",
         "the reason names queueing delay");

  // A drop count with a comfortable queue delay is not pressure: the delay is
  // measured and says the host is fine.
  molink::stream_qos measured;
  auto ok = healthy();
  ok.dropped = 7;
  ok.queue_delay_ms = 20;
  for (int i = 0; i < 6; ++i) {
    expect(!measured.note(ok, static_cast<uint64_t>(i) * 1000),
           "viewer drops are ignored when the queue delay is healthy");
  }
  expect(measured.bitrate_scale() == 1.0, "no downgrade from a healthy queue");
}

void test_static_screen_never_moves() {
  molink::stream_qos qos;
  auto in = healthy();
  in.demand = false;
  in.paint_fps = 0;
  for (int i = 0; i < 30; ++i) {
    expect(!qos.note(in, static_cast<uint64_t>(i) * 1000),
           "a still screen says nothing about the link");
  }
  expect(qos.bitrate_scale() == 1.0, "no drift while nobody paints");
}

void test_recovery_is_proportional_and_dwelled() {
  molink::stream_qos qos;
  const auto bad = stalled();
  for (uint64_t t = 0; t <= 40000; t += 1000) qos.note(bad, t);
  expect(qos.bitrate_scale() <= molink::stream_qos::kMinScale + 1e-9,
         "a long stall reaches the floor");
  expect(qos.force_cbr(), "the floor pins constant bitrate");

  const auto good = healthy();
  const double before = qos.bitrate_scale();
  bool saw_up = false;
  uint64_t up_at = 0;
  for (int i = 0; i < 6; ++i) {
    const uint64_t now = 100000 + static_cast<uint64_t>(i) * 1000;
    if (qos.note(good, now) && qos.bitrate_scale() > before) {
      saw_up = true;
      up_at = now;
      break;
    }
  }
  expect(saw_up, "a healthy window lets the budget climb");
  expect(close_to(qos.bitrate_scale(),
                  (std::min)(molink::stream_qos::kMaxScale,
                             before * molink::stream_qos::kUpStep)),
         "the up step is the 1.12 ratio");
  expect(up_at <= 103000, "recovery does not wait five seconds for a step");
  expect(qos.last_change() == molink::stream_qos::change::up,
         "the move is reported as an up");
  expect(std::string(qos.last_reason()) == "healthy",
         "the recovery reason is healthy");

  for (int i = 0; i < 80; ++i) {
    qos.note(good, 200000 + static_cast<uint64_t>(i) * 1000);
  }
  expect(!qos.force_cbr(), "a healthy streak releases the hard ceiling");
  expect(qos.bitrate_scale() >= 0.999,
         "the budget springs back towards the user's anchor");
  expect(qos.qp_offset() == 0, "the extra QP roughness is given back");
}

void test_rtt_alone_needs_two_windows() {
  molink::stream_qos qos;
  auto in = healthy();
  in.queue_delay_ms = 0;
  in.rtt_ms = 250;  // well past the interactive threshold
  expect(!qos.note(in, 0), "one high-RTT window is ignored");
  expect(qos.note(in, 1000), "sustained high RTT shaves the budget");
  expect(qos.bitrate_scale() < 1.0, "high RTT alone shaves the budget");
  expect(!qos.force_cbr(), "high RTT alone does not pin CBR");
  expect(std::string(qos.last_reason()) == "rtt",
         "the reason names RTT");
}

}  // namespace

int main() {
  test_healthy_is_quiet();
  test_stall_needs_two_windows_and_dwells();
  test_dropped_fallback_requires_consecutive_windows();
  test_queue_delay_is_the_preferred_signal();
  test_static_screen_never_moves();
  test_recovery_is_proportional_and_dwelled();
  test_rtt_alone_needs_two_windows();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
