// The rate-driven quality controller: the preset is where the picture starts,
// and the viewer's painted frame rate decides whether it moves.
#include <cstdio>

#include "capture/frame_budget.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

// Steps the policy with the same sample repeated until the dwell elapses.
bool hold(molink::rate_quality_policy& policy, double fps, int target,
          bool demand, bool auto_mode, int auto_levels, int steps,
          uint64_t& now) {
  bool changed = false;
  for (int i = 0; i < steps; ++i) {
    now += 500;
    changed = policy.note(fps, target, demand, auto_mode, auto_levels, now) ||
              changed;
  }
  return changed;
}

}  // namespace

int main() {
  const uint64_t t0 = 10000;

  // 1. A static screen produces no demand: nothing moves, whatever the rate.
  {
    molink::rate_quality_policy policy;
    uint64_t now = t0;
    expect(!hold(policy, 0.5, 15, /*demand=*/false, false, 5, 20, now),
           "no demand never changes quality");
    expect(policy.qp_offset == 0, "no demand keeps the preset's own QP");
  }

  // 2. Behind the target: coarsen in steps after the dwell, up to the bound.
  {
    molink::rate_quality_policy policy;
    uint64_t now = t0;
    expect(!policy.note(4.0, 15, true, false, 5, now),
           "the first behind sample only starts the dwell");
    hold(policy, 4.0, 15, true, false, 5, 3, now);
    expect(policy.qp_offset == 2, "a sustained low rate coarsens by two steps");
    hold(policy, 4.0, 15, true, false, 5, 100, now);
    expect(policy.qp_offset == molink::rate_quality_policy::kMaxCoarserSteps,
           "coarsening stops at the bound");
  }

  // 3. Healthy: sharpen one step at a time after the longer dwell, but never
  //    past the sharper bound.
  {
    molink::rate_quality_policy policy;
    uint64_t now = t0;
    policy.qp_offset = 4;
    hold(policy, 14.0, 15, true, false, 5, 3, now);
    expect(policy.qp_offset == 4, "the healthy dwell is longer than the bad one");
    hold(policy, 14.0, 15, true, false, 5, 20, now);
    expect(policy.qp_offset < 4, "a healthy link reclaims sharpness");
    hold(policy, 20.0, 15, true, false, 5, 200, now);
    expect(policy.qp_offset == -molink::rate_quality_policy::kMaxSharperSteps,
           "sharpening stops at the bound");
  }

  // 4. A rate inside the band changes nothing and resets the dwells.
  {
    molink::rate_quality_policy policy;
    uint64_t now = t0;
    hold(policy, 12.0, 15, true, false, 5, 40, now);
    expect(policy.qp_offset == 0, "a rate near the target keeps the preset");
  }

  // 5. Auto walks the ladder instead of the QP offset, and never leaves it.
  {
    molink::rate_quality_policy policy;
    uint64_t now = t0;
    policy.auto_level = 2;
    hold(policy, 3.0, 15, true, true, 5, 30, now);
    expect(policy.auto_level == 0, "auto falls to the coarsest level it may use");
    expect(policy.qp_offset == 0, "auto does not move the QP offset");
    hold(policy, 22.0, 15, true, true, 5, 100, now);
    expect(policy.auto_level == 4, "auto climbs to the sharpest level");
  }

  // 6. A missing viewer report is not "behind": nothing moves.
  {
    molink::rate_quality_policy policy;
    uint64_t now = t0;
    hold(policy, 0.0, 15, true, false, 5, 20, now);
    expect(policy.qp_offset == 0, "a viewer that reports nothing is left alone");
  }

  std::printf("rate quality: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
