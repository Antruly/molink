// Weight-based stream QoS: the controller must squeeze the budget when the
// slowest viewer stalls, pin a hard ceiling once quality control has failed,
// and walk back to constant quality only after a healthy streak.
#include <cstdio>

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

molink::stream_qos::input healthy() {
  molink::stream_qos::input in;
  in.paint_fps = 30;
  in.target_fps = 30;
  in.dropped = 0;
  in.rtt_ms = 20;
  in.demand = true;
  return in;
}

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
}

void test_stall_squeezes_then_pins_cbr() {
  molink::stream_qos qos;
  const auto in = stalled();
  qos.note(in, 0);
  expect(qos.bitrate_scale() < 0.8, "one stalled window squeezes the budget");
  for (int i = 1; i <= 6; ++i) qos.note(in, static_cast<uint64_t>(i) * 1000);
  expect(qos.bitrate_scale() <= molink::stream_qos::kMinScale + 1e-9,
         "the scale reaches its floor");
  expect(qos.force_cbr(), "constant bitrate once the floor is reached");
  for (int i = 7; i <= 12; ++i) qos.note(in, static_cast<uint64_t>(i) * 1000);
  expect(qos.qp_offset() > 0, "still stalling coarsens the QP");
  expect(qos.qp_offset() <= molink::stream_qos::kMaxQpOffset,
         "the QP offset stays bounded");
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

void test_recovery_releases_the_ceiling() {
  molink::stream_qos qos;
  const auto bad = stalled();
  for (int i = 0; i < 12; ++i) qos.note(bad, static_cast<uint64_t>(i) * 1000);
  expect(qos.force_cbr(), "the ceiling is on after a long stall");
  const auto good = healthy();
  uint64_t now = 20000;
  int rounds = 0;
  while (qos.force_cbr() && rounds < 120) {
    qos.note(good, now);
    now += 1000;
    ++rounds;
  }
  expect(!qos.force_cbr(), "a healthy streak releases the hard ceiling");
  expect(qos.bitrate_scale() >= 0.999,
         "the budget springs back towards the user's anchor");
  expect(qos.qp_offset() == 0, "the extra QP roughness is given back");
}

void test_rtt_alone_shaves() {
  molink::stream_qos qos;
  auto in = healthy();
  in.rtt_ms = 180;  // above the interactive threshold, below a stall
  qos.note(in, 0);
  expect(qos.bitrate_scale() < 1.0, "high RTT alone shaves the budget");
  expect(!qos.force_cbr(), "high RTT alone does not pin CBR");
}

}  // namespace

int main() {
  test_healthy_is_quiet();
  test_stall_squeezes_then_pins_cbr();
  test_static_screen_never_moves();
  test_recovery_releases_the_ceiling();
  test_rtt_alone_shaves();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
