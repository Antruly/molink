#include <cstdio>

#include "capture/frame_budget.h"

namespace {
int failures = 0;
int checks = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void test_budget() {
  using molink::frame_budget_us;
  expect(frame_budget_us(15) == 56666, "15 fps leaves 85 % of 66.7 ms");
  expect(frame_budget_us(5) == 170000, "5 fps budget is 170 ms");
  expect(frame_budget_us(1) == 850000, "1 fps budget is 850 ms");
  expect(frame_budget_us(0) == frame_budget_us(1), "0 fps clamps to 1 fps");
  expect(frame_budget_us(-4) == frame_budget_us(1),
         "negative fps clamps to 1 fps");
}

void test_overrun_raises_qp() {
  using molink::adapt_av1_qp_offset;
  using molink::kMaxAv1QpOffset;
  const unsigned long long budget = 170000;  // 5 fps
  expect(adapt_av1_qp_offset(0, 230000, budget) == 2,
         "a severe overrun raises the QP by two steps");
  expect(adapt_av1_qp_offset(0, 180000, budget) == 1,
         "a small overrun raises the QP by one step");
  expect(adapt_av1_qp_offset(kMaxAv1QpOffset, 900000, budget) ==
             kMaxAv1QpOffset,
         "the offset never exceeds its bound");
}

void test_healthy_link_reclaims_quality() {
  using molink::adapt_av1_qp_offset;
  const unsigned long long budget = 170000;
  expect(adapt_av1_qp_offset(6, 40000, budget) == 5,
         "a frame well inside the budget lowers the offset");
  expect(adapt_av1_qp_offset(0, 1000, budget) == 0,
         "the offset never goes below zero");
  expect(adapt_av1_qp_offset(6, 120000, budget) == 6,
         "a frame inside the budget but not by half keeps the offset");
}

void test_at_the_limit() {
  using molink::adapt_av1_qp_offset;
  const unsigned long long budget = 170000;
  expect(adapt_av1_qp_offset(3, budget, budget) == 3,
         "exactly the budget neither raises nor lowers");
  expect(adapt_av1_qp_offset(3, 0, 0) == 3,
         "a zero budget leaves the offset alone");
}

void test_policy_suspends_after_a_streak() {
  molink::av1_budget_policy policy;
  expect(!policy.note_frame(true, true, 0), "one slow frame keeps AV1");
  expect(!policy.note_frame(true, true, 10), "two slow frames keep AV1");
  expect(policy.note_frame(true, true, 20),
         "the third slow frame at the QP bound drops AV1");
  expect(policy.suspended, "the policy is suspended");
  expect(policy.suspend_until_ms == 20 + policy.kInitialCooldownMs,
         "the first cooldown is the initial one");
  expect(policy.cooldown_ms == policy.kInitialCooldownMs * 2,
         "the next cooldown is already doubled");
}

void test_policy_needs_the_qp_bound() {
  molink::av1_budget_policy policy;
  for (int i = 0; i < 10; ++i) {
    expect(!policy.note_frame(true, false, static_cast<uint64_t>(i * 10)),
           "AV1 is kept while the QP can still rise");
  }
  expect(!policy.suspended, "quality is traded before AV1 is dropped");
}

void test_policy_streak_resets() {
  molink::av1_budget_policy policy;
  policy.note_frame(true, true, 0);
  policy.note_frame(false, true, 10);  // a fast frame breaks the streak
  policy.note_frame(true, true, 20);
  expect(!policy.note_frame(true, true, 30),
         "a broken streak starts counting again");
  expect(!policy.suspended, "two slow frames after a fast one are not enough");
}

void test_policy_retries_after_cooldown() {
  molink::av1_budget_policy policy;
  for (int i = 0; i < 3; ++i) policy.note_frame(true, true, 100);
  bool resumed = false;
  expect(!policy.allow_av1(1000, resumed), "a suspension is honoured");
  expect(!resumed, "a blocked frame is not a retry");
  expect(policy.allow_av1(policy.suspend_until_ms, resumed),
         "the retry runs when the cooldown ends");
  expect(resumed, "the caller learns the retry just started");
  expect(policy.trial_started_ms == policy.suspend_until_ms,
         "the retry remembers when it started");
}

void test_policy_settled_retry_restores_the_cooldown() {
  molink::av1_budget_policy policy;
  for (int i = 0; i < 3; ++i) policy.note_frame(true, true, 0);
  bool resumed = false;
  policy.allow_av1(policy.suspend_until_ms, resumed);
  const uint64_t started = policy.trial_started_ms;
  expect(!policy.note_frame(false, true, started + policy.kSettledMs),
         "a frame inside the budget after the settle window keeps AV1");
  expect(policy.cooldown_ms == policy.kInitialCooldownMs,
         "a retry that held restores the short cooldown");
}

void test_policy_reset() {
  molink::av1_budget_policy policy;
  for (int i = 0; i < 3; ++i) policy.note_frame(true, true, 0);
  policy.reset();
  expect(!policy.suspended, "reset clears the suspension");
  expect(policy.cooldown_ms == policy.kInitialCooldownMs,
         "reset restores the initial cooldown");
}
}  // namespace

int main() {
  test_budget();
  test_overrun_raises_qp();
  test_healthy_link_reclaims_quality();
  test_at_the_limit();
  test_policy_suspends_after_a_streak();
  test_policy_needs_the_qp_bound();
  test_policy_streak_resets();
  test_policy_retries_after_cooldown();
  test_policy_settled_retry_restores_the_cooldown();
  test_policy_reset();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
