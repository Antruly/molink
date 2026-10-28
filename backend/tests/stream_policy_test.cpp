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

void test_enters_after_one_second_of_movement() {
  molink::stream_policy policy;
  expect(!policy.note(0.30, 0), "the first moving frame only starts the clock");
  expect(!policy.note(0.30, 500), "half a second is not enough");
  expect(policy.note(0.30, 1000), "one second of movement enters the stream");
  expect(policy.streaming, "the policy is streaming");
  expect(policy.armed, "movement keeps the encoder armed");
}

void test_still_screen_never_enters() {
  molink::stream_policy policy;
  for (uint64_t t = 0; t < 5000; t += 200) {
    expect(!policy.note(0.05, t), "a mostly still screen stays on tiles");
  }
  expect(!policy.streaming, "no stream on a still screen");
  expect(!policy.armed, "a still screen is not armed");
}

void test_short_burst_does_not_enter() {
  molink::stream_policy policy;
  policy.note(0.40, 0);
  policy.note(0.40, 400);
  expect(!policy.note(0.02, 800), "a short burst does not enter the stream");
  expect(!policy.streaming, "mode unchanged after the burst");
}

void test_leaves_after_ten_seconds_still() {
  molink::stream_policy policy;
  policy.note(0.40, 0);
  policy.note(0.40, 1000);  // enters
  expect(policy.streaming, "streaming after one second");
  expect(!policy.note(0.01, 3000), "three seconds still is not enough");
  expect(!policy.note(0.01, 12000), "nine seconds still is not enough");
  expect(policy.note(0.01, 13000), "ten seconds still leaves the stream");
  expect(!policy.streaming, "back on tiles");
}

void test_movement_resets_the_exit_clock() {
  molink::stream_policy policy;
  policy.note(0.40, 0);
  policy.note(0.40, 1000);
  policy.note(0.01, 5000);    // starts the exit clock
  policy.note(0.50, 8000);    // movement again: exit clock restarts
  expect(!policy.note(0.01, 12000), "the exit dwell restarts after movement");
  expect(policy.streaming, "still streaming after a fresh still window");
  expect(policy.note(0.01, 22000), "ten quiet seconds after the last movement");
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
}
}  // namespace

int main() {
  test_enters_after_one_second_of_movement();
  test_still_screen_never_enters();
  test_short_burst_does_not_enter();
  test_leaves_after_ten_seconds_still();
  test_movement_resets_the_exit_clock();
  test_video_codec_choice();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
