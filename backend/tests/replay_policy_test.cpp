#include <cstdio>
#include <string>

#include "capture/replay_policy.h"

namespace {
int failures = 0;
int checks = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void test_normal_replay() {
  using molink::choose_replay_action;
  using molink::replay_action;
  expect(choose_replay_action(60, 1000000, true) == replay_action::replay,
         "exact frame and age limits are still replayable");
  expect(choose_replay_action(12, 800000, true) == replay_action::replay,
         "small recent backlog is replayed");
}

void test_slow_client_skips() {
  using molink::choose_replay_action;
  using molink::replay_action;
  expect(choose_replay_action(61, 100000, true) ==
             replay_action::skip_to_latest,
         "initialized client skips when backlog exceeds frame limit");
  expect(choose_replay_action(10, 1000001, true) ==
             replay_action::skip_to_latest,
         "initialized client skips when oldest frame exceeds age limit");
}

void test_new_client_requests_keyframe() {
  using molink::choose_replay_action;
  using molink::replay_action;
  expect(choose_replay_action(61, 0, false) ==
             replay_action::request_keyframe,
         "client without a base image requests a keyframe");
  expect(choose_replay_action(1, 1000001, false) ==
             replay_action::request_keyframe,
         "old pending frame cannot bootstrap a new client");
}
}  // namespace

int main() {
  test_normal_replay();
  test_slow_client_skips();
  test_new_client_requests_keyframe();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
