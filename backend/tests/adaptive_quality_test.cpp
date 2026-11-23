// The auto tier ladder, hysteresis, dwell and frame-rate controller. These
// are the same numbers the Web runs, so the checks pin the parity.
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "adaptive_quality.h"

namespace {

using namespace molink::adaptive;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_eq(int actual, int expected, const char* description) {
  ++checks;
  if (actual == expected) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s (expected %d, got %d)\n", description,
               expected, actual);
}

sample good_sample(tier current) {
  sample value;
  value.latency_known = true;
  value.latency_ms = 50;
  value.wire_fps = 20.0;
  value.dropped = 0;
  value.bandwidth_bps = 1024.0 * 1024.0;
  value.target_fps = plan_for(current).start;
  return value;
}

sample bad_sample(tier current) {
  sample value;
  value.latency_known = true;
  value.latency_ms = 400;
  value.wire_fps = 2.0;
  value.dropped = 9;
  value.bandwidth_bps = 128.0 * 1024.0;
  value.target_fps = plan_for(current).floor;
  return value;
}

void test_plans_match_web() {
  const fps_plan turbo = plan_for(tier::turbo);
  expect_eq(turbo.min, 5, "turbo min");
  expect_eq(turbo.floor, 5, "turbo floor");
  expect_eq(turbo.max, 25, "turbo max");
  expect_eq(turbo.start, 15, "turbo start");

  const fps_plan smooth = plan_for(tier::smooth);
  expect_eq(smooth.floor, 10, "smooth floor");
  expect_eq(smooth.max, 25, "smooth max");

  const fps_plan balanced = plan_for(tier::balanced);
  expect_eq(balanced.floor, 10, "balanced floor");
  expect_eq(balanced.start, 15, "balanced start");

  for (tier value : {tier::hd, tier::truecolor}) {
    const fps_plan plan = plan_for(value);
    expect_eq(plan.min, 5, "sharp tier min");
    expect_eq(plan.floor, 15, "sharp tier floor");
    expect_eq(plan.max, 15, "sharp tier max");
    expect_eq(plan.start, 15, "sharp tier start");
  }
  expect_eq(index_of(tier_at(0)), 0, "tier 0 round-trips");
  expect_eq(index_of(tier_at(4)), 4, "tier 4 round-trips");
  expect_eq(index_of(tier_at(99)), 4, "tier index clamps high");
}

void test_tier_down_needs_three_samples_and_dwell() {
  tier_state state;
  state.current = tier::balanced;
  state.last_change_ms = 0;
  const sample bad = bad_sample(tier::balanced);

  expect(!step_tier(&state, bad, 1000).changed, "first bad sample waits");
  expect(!step_tier(&state, bad, 2000).changed, "second bad sample waits");
  expect(!step_tier(&state, bad, 3000).changed,
         "third bad sample waits for the 6 s dwell");
  const tier_step down = step_tier(&state, bad, 6000);
  expect(down.changed, "a stepped-down tier after the dwell");
  expect(down.value == tier::smooth, "balanced steps down to smooth");
  expect(state.current == tier::smooth, "state keeps the new tier");
}

void test_bad_samples_do_not_downgrade_above_the_floor() {
  tier_state state;
  state.current = tier::balanced;
  state.last_change_ms = 0;
  sample bad = bad_sample(tier::balanced);
  bad.target_fps = 20;  // still above the tier's 10 fps floor
  for (uint64_t now = 1000; now <= 30000; now += 1000) {
    expect(!step_tier(&state, bad, now).changed,
           "a tier only steps down once the frame rate is at its floor");
  }
}

void test_tier_up_needs_five_samples_and_dwell() {
  tier_state state;
  state.current = tier::smooth;
  state.last_change_ms = 0;
  const sample good = good_sample(tier::smooth);

  for (uint64_t now = 1000; now < 10000; now += 1000) {
    expect(!step_tier(&state, good, now).changed,
           "tier up waits for five samples and the 10 s dwell");
  }
  const tier_step up = step_tier(&state, good, 10000);
  expect(up.changed, "tier up fires after the dwell");
  expect(up.value == tier::balanced, "smooth steps up to balanced");
}

void test_tier_up_rejects_unknown_latency_and_wide_bandwidth() {
  tier_state state;
  state.current = tier::smooth;
  state.last_change_ms = 0;
  sample good = good_sample(tier::smooth);
  good.latency_known = false;
  for (uint64_t now = 1000; now <= 30000; now += 1000) {
    expect(!step_tier(&state, good, now).changed,
           "unknown RTT is not healthy");
  }

  tier_state wide;
  wide.current = tier::smooth;
  wide.last_change_ms = 0;
  sample loaded = good_sample(tier::smooth);
  loaded.bandwidth_bps = 13.0 * 1024.0 * 1024.0;
  for (uint64_t now = 1000; now <= 30000; now += 1000) {
    expect(!step_tier(&wide, loaded, now).changed,
           "auto does not climb while the link is over 12 MiB/s");
  }
}

void test_tier_cooldown_blocks_back_to_back_changes() {
  tier_state state;
  state.current = tier::balanced;
  state.last_change_ms = 0;
  const sample bad = bad_sample(tier::balanced);
  for (uint64_t now = 1000; now <= 6000; now += 1000) {
    step_tier(&state, bad, now);
  }
  expect(state.current == tier::smooth, "the first downgrade landed");
  for (uint64_t now = 7000; now < 12000; now += 1000) {
    expect(!step_tier(&state, bad, now).changed,
           "the 6 s cooldown blocks an immediate second downgrade");
  }
  expect(step_tier(&state, bad, 12000).changed,
         "the ladder moves again once the cooldown expires");
  expect(state.current == tier::turbo, "smooth steps down to turbo");
}

void test_tier_recovers_symmetrically_at_the_top() {
  tier_state state;
  state.current = tier::truecolor;
  state.last_change_ms = 0;
  const sample good = good_sample(tier::truecolor);
  for (uint64_t now = 1000; now <= 30000; now += 1000) {
    expect(!step_tier(&state, good, now).changed,
           "the top rung has nowhere to climb");
  }
}

void test_fps_downgrade_and_cooldown() {
  fps_state state;
  state.target = 15;
  state.last_change_ms = 0;
  const fps_plan plan = plan_for(tier::balanced);
  sample bad = bad_sample(tier::balanced);
  bad.latency_ms = 300;
  bad.wire_fps = 15.0;
  bad.dropped = 3;
  bad.target_fps = 15;

  expect(!step_fps(&state, bad, plan, 100).changed, "first bad fps sample waits");
  expect(!step_fps(&state, bad, plan, 200).changed,
         "second bad sample still waits for the 1.5 s dwell");
  const fps_step down = step_fps(&state, bad, plan, 1600);
  expect(down.changed, "the frame rate drops after the dwell");
  expect_eq(down.target, 11, "15 fps steps to 11 fps");
  expect(!step_fps(&state, bad, plan, 2000).changed,
         "the cooldown blocks an immediate second drop");
  const fps_step again = step_fps(&state, bad, plan, 3200);
  expect(again.changed, "the second drop lands after 1.5 s");
  expect_eq(again.target, 8, "11 fps steps to 8 fps");
  expect(plan.min <= again.target, "the plan floor is respected");
}

void test_fps_underrun_jumps_to_the_wire_rate() {
  fps_state state;
  state.target = 25;
  state.last_change_ms = 0;
  const fps_plan plan = plan_for(tier::balanced);
  sample bad;
  bad.latency_known = true;
  bad.latency_ms = 50;
  bad.wire_fps = 10.0;  // below 60 % of 25
  bad.target_fps = 25;
  step_fps(&state, bad, plan, 100);
  const fps_step step = step_fps(&state, bad, plan, 1600);
  expect(step.changed, "an underrunning link drops the rate");
  expect_eq(step.target, 10, "the rate jumps straight to the wire rate");
}

void test_fps_cpu_ceiling() {
  fps_state state;
  state.target = 15;
  state.last_change_ms = 0;
  const fps_plan plan = plan_for(tier::balanced);
  sample bad;
  bad.latency_known = true;
  bad.latency_ms = 50;
  bad.wire_fps = 15.0;
  bad.host_build_ms = 100.0;  // 850 / 100 = 8 fps ceiling
  bad.target_fps = 15;
  step_fps(&state, bad, plan, 100);
  const fps_step step = step_fps(&state, bad, plan, 1600);
  expect(step.changed, "a slow host lowers the target");
  expect_eq(step.target, 8, "the target never exceeds the encode ceiling");
}

void test_fps_recovers_slowly_not_past_the_ceiling() {
  fps_state state;
  state.target = 5;
  state.last_change_ms = 0;
  const fps_plan plan = plan_for(tier::balanced);
  sample good;
  good.latency_known = true;
  good.latency_ms = 50;
  good.wire_fps = 20.0;
  good.target_fps = 5;
  good.host_build_ms = 10.0;
  for (uint64_t now = 1000; now < 4000; now += 1000) {
    expect(!step_fps(&state, good, plan, now).changed,
           "recovery needs four good samples and 3 s");
  }
  const fps_step up = step_fps(&state, good, plan, 4000);
  expect(up.changed, "recovery fires after the dwell");
  expect_eq(up.target, 10, "5 fps climbs by 5 fps");
}

void test_reset_starts_at_balanced() {
  tier_state tiers;
  tiers.current = tier::truecolor;
  fps_state fps;
  fps.target = 60;
  reset_auto(&tiers, &fps, 1234);
  expect(tiers.current == tier::balanced, "auto starts on balanced");
  expect_eq(fps.target, 15, "auto starts at the balanced resting rate");
  expect(tiers.last_change_ms == 1234 && fps.last_change_ms == 1234,
         "the dwell clock starts at the switch");
}

}  // namespace

int main() {
  test_plans_match_web();
  test_tier_down_needs_three_samples_and_dwell();
  test_bad_samples_do_not_downgrade_above_the_floor();
  test_tier_up_needs_five_samples_and_dwell();
  test_tier_up_rejects_unknown_latency_and_wide_bandwidth();
  test_tier_cooldown_blocks_back_to_back_changes();
  test_tier_recovers_symmetrically_at_the_top();
  test_fps_downgrade_and_cooldown();
  test_fps_underrun_jumps_to_the_wire_rate();
  test_fps_cpu_ceiling();
  test_fps_recovers_slowly_not_past_the_ceiling();
  test_reset_starts_at_balanced();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
