#include <cstdio>

#include "capture/stream_policy.h"

namespace {
int failures = 0;
int checks = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void test_enters_after_enter_dwell() {
  molink::stream_policy policy;
  expect(!policy.note(0.30, 0), "the first moving frame only starts the clock");
  expect(!policy.note(0.30, 499), "just under the dwell is not enough");
  expect(policy.note(0.30, 500), "500 ms of averaged movement enters the stream");
  expect(policy.streaming, "the policy is streaming");
  expect(policy.armed, "movement keeps the encoder armed");
  expect(policy.epoch() == 1, "the switch bumped stream_epoch_id");
}

void test_still_screen_never_enters() {
  molink::stream_policy policy;
  for (uint64_t t = 0; t < 5000; t += 200) {
    expect(!policy.note(0.05, t), "a mostly still screen stays on tiles");
  }
  expect(!policy.streaming, "no stream on a still screen");
  expect(!policy.armed, "a still screen is not armed");
  expect(policy.epoch() == 0, "no epoch change without a switch");
}

void test_short_burst_does_not_enter() {
  molink::stream_policy policy;
  // One high frame, then a minute fraction. The 500 ms window drains the spike
  // long before the enter dwell expires.
  policy.note(0.50, 0);
  for (uint64_t t = 50; t <= 2000; t += 50) {
    expect(!policy.note(0.01, t), "a short burst does not enter the stream");
  }
  expect(!policy.streaming, "mode unchanged after the burst");
  expect(policy.epoch() == 0, "a rejected burst does not bump the epoch");
}

void test_leaves_after_exit_dwell() {
  molink::stream_policy policy;
  policy.note(0.40, 0);
  policy.note(0.40, 500);  // enters
  expect(policy.streaming, "streaming after the enter dwell");
  uint64_t switched_at = 0;
  for (uint64_t t = 600; t <= 4000; t += 100) {
    if (policy.note(0.0, t)) {
      switched_at = t;
      break;
    }
  }
  expect(switched_at != 0, "the still picture eventually leaves the stream");
  expect(switched_at >= 3000 && switched_at <= 3500,
         "the exit waited for the 2.5 s dwell");
  expect(!policy.streaming, "back on tiles");
  expect(policy.epoch() == 2, "the exit bumped stream_epoch_id too");
}

void test_hysteresis_keeps_the_stream() {
  molink::stream_policy policy;
  policy.note(0.40, 0);
  policy.note(0.40, 500);
  expect(policy.streaming, "entered the stream");
  // Between the exit (9 %) and enter (15 %) thresholds: no edge to oscillate
  // on. The old single-threshold policy switched back and forth here.
  for (uint64_t t = 600; t <= 10000; t += 100) {
    expect(!policy.note(0.12, t), "the hysteresis band keeps the mode");
  }
  expect(policy.streaming, "still streaming inside the hysteresis band");
}

void test_movement_resets_the_exit_clock() {
  molink::stream_policy policy;
  policy.note(0.40, 0);
  policy.note(0.40, 500);
  for (uint64_t t = 600; t <= 3000; t += 100) {
    policy.note(0.0, t);  // starts the exit clock
  }
  expect(!policy.note(0.60, 3100), "movement restarts the exit clock");
  expect(policy.streaming, "still streaming after a fresh movement");
  for (uint64_t t = 3200; t < 6200; t += 100) {
    expect(!policy.note(0.0, t), "the 2.5 s still window restarts");
  }
  expect(policy.note(0.0, 6200), "2.5 quiet seconds after the last movement");
}

void test_cooldown_blocks_an_immediate_switch_back() {
  molink::stream_policy policy;
  policy.set_timing(500, 100, 100, 5000);
  policy.note(0.40, 0);
  expect(policy.note(0.40, 100), "enters quickly with a short dwell");
  // The exit dwell is satisfied 100 ms later, but the 5 s cooldown is not.
  for (uint64_t t = 200; t <= 5000; t += 100) {
    expect(!policy.note(0.01, t), "cooldown blocks the switch back");
  }
  expect(policy.note(0.01, 5100), "after the cooldown the exit is allowed");
}

void test_veto_drops_the_dwell() {
  molink::stream_policy policy;
  policy.note(0.40, 0);
  policy.note(0.40, 500);
  expect(policy.streaming, "entered the stream");
  // A drag or a backed-up send vetoes switching. The latches are dropped, so
  // the instant the veto lifts does not switch on a stale dwell.
  for (uint64_t t = 600; t <= 5000; t += 100) {
    expect(!policy.note(0.01, t, /*allow_switch=*/false),
           "a vetoed policy never switches");
  }
  expect(policy.streaming, "the veto keeps the current mode");
  expect(!policy.note(0.01, 5100), "the exit dwell restarts after the veto");
  expect(policy.note(0.01, 7600), "2.5 s after the veto the exit proceeds");
}

void test_slow_encoder_only_blocks_entering() {
  using molink::stream_switch_allowed;
  // A stream encoder over its budget must not be entered: the picture would
  // stutter. It must, however, be allowed to leave, because leaving is the
  // repair -- tiles do not pay the encoder cost. Vetoing both directions kept
  // a slow host streaming at a few frames per second forever.
  expect(!stream_switch_allowed(/*dragging=*/false, /*congested=*/false,
                                /*any_stream_codec=*/true,
                                /*streaming=*/false,
                                /*encoder_blocked=*/true),
         "a slow encoder keeps the policy out of the stream");
  expect(stream_switch_allowed(false, false, true, true, true),
         "a slow encoder must not pin an existing stream");
  expect(!stream_switch_allowed(true, false, true, true, false),
         "a drag vetoes every switch");
  expect(!stream_switch_allowed(false, true, true, true, false),
         "a refused send vetoes every switch");
  expect(!stream_switch_allowed(false, false, false, true, false),
         "no decodable codec vetoes every switch");
  expect(stream_switch_allowed(false, false, true, true, false),
         "a healthy streaming session may leave");
  expect(stream_switch_allowed(false, false, true, false, false),
         "a healthy idle session may enter");
}

void test_force_and_rollback() {
  molink::stream_policy policy;
  expect(policy.force(true, 1000), "force enters the stream");
  expect(policy.streaming, "forced streaming");
  expect(policy.epoch() == 1, "force bumps the epoch");
  policy.rollback(1100);
  expect(!policy.streaming, "rollback returns to tiles");
  expect(policy.epoch() == 2, "rollback bumps the epoch");
  // A rollback is a switch for cooldown purposes: no immediate re-entry.
  policy.note(0.40, 1200);
  expect(!policy.note(0.40, 1800), "the cooldown applies after a rollback");
}

void test_video_codec_choice() {
  using molink::stream_choice;
  // AV1 whenever every viewer can decode it: it compresses far better.
  expect(molink::choose_video_codec(true, true, true) == stream_choice::av1,
         "all viewers on AV1 choose AV1");
  expect(molink::choose_video_codec(true, true, false) == stream_choice::av1,
         "AV1 does not need the H.264 capability");
  // A viewer without AV1 still gets a stream when everyone can take H.264.
  expect(molink::choose_video_codec(false, true, true) ==
             stream_choice::h264,
         "AV1 switched off falls back to H.264");
  expect(molink::choose_video_codec(true, false, true) == stream_choice::h264,
         "one AV1-less viewer moves the stream to H.264");
  // Mixed decoders: nothing both can take.
  expect(molink::choose_video_codec(true, false, false) ==
             stream_choice::none,
         "viewers with no shared codec stay on tiles");
  expect(molink::choose_video_codec(false, false, false) ==
             stream_choice::none,
         "no decoder at all stays on tiles");
  // Without a hardware AV1 encoder, CPU AV1 is ranked behind every other
  // stream: a few frames per second reads as a frozen picture.
  expect(molink::choose_video_codec(true, true, true, true, true, false) ==
             stream_choice::vp9,
         "CPU-only AV1 yields to VP9");
  expect(molink::choose_video_codec(true, true, true, false, true, false) ==
             stream_choice::vp8,
         "CPU-only AV1 yields to VP8");
  expect(molink::choose_video_codec(true, true, true, false, false, false) ==
             stream_choice::h264,
         "CPU-only AV1 yields to H.264");
  expect(molink::choose_video_codec(true, true, false, false, false, false) ==
             stream_choice::av1,
         "CPU-only AV1 is still used when nothing else is shared");
  expect(molink::choose_video_codec(true, true, true, true, true, true) ==
             stream_choice::av1,
         "a hardware AV1 encoder keeps AV1 first");
}
}  // namespace

int main() {
  test_enters_after_enter_dwell();
  test_still_screen_never_enters();
  test_short_burst_does_not_enter();
  test_leaves_after_exit_dwell();
  test_hysteresis_keeps_the_stream();
  test_movement_resets_the_exit_clock();
  test_cooldown_blocks_an_immediate_switch_back();
  test_veto_drops_the_dwell();
  test_slow_encoder_only_blocks_entering();
  test_force_and_rollback();
  test_video_codec_choice();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
