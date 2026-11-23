#include "adaptive_quality.h"

#include <algorithm>
#include <cmath>

namespace molink::adaptive {
namespace {

// Auto windows per tier, mapped from the Web's `AUTO_PLANS`:
//   turbo -> turbo, smooth -> smooth, balanced -> balanced,
//   hd -> quality, truecolor -> lossless.
// The absolute floor stays 5 for every tier: a host that cannot encode, or a
// link that cannot carry the picture, always wins over the resting rate.
constexpr fps_plan kPlans[tier_count] = {
    {5, 5, 25, 15},   // turbo
    {5, 10, 25, 15},  // smooth
    {5, 10, 25, 15},  // balanced
    {5, 15, 15, 15},  // hd
    {5, 15, 15, 15},  // truecolor
};

int index_value(tier value) {
  const int index = static_cast<int>(value);
  return index < 0 ? 0 : (index >= tier_count ? tier_count - 1 : index);
}

}  // namespace

fps_plan plan_for(tier value) { return kPlans[index_value(value)]; }

tier tier_at(int index) {
  if (index <= 0) return tier::turbo;
  if (index >= tier_count) return tier::truecolor;
  return static_cast<tier>(index);
}

int index_of(tier value) { return index_value(value); }

int clamp_fps(int value) {
  if (value < min_fps) return min_fps;
  if (value > max_fps) return max_fps;
  return value;
}

tier_step step_tier(tier_state* state, const sample& value, uint64_t now_ms) {
  tier_step result;
  if (state == nullptr) return result;
  result.value = state->current;

  const fps_plan plan = plan_for(state->current);
  const int target = value.target_fps;
  const double wire = value.wire_fps;
  const bool underrunning =
      wire > 0.0 && wire < static_cast<double>(std::max(3, target / 2));
  const bool bad = (value.latency_known && value.latency_ms > kLatencyBadMs) ||
                   underrunning || value.dropped > kDroppedBad;
  const bool good =
      value.latency_known && value.latency_ms < kLatencyGoodMs &&
      value.dropped == 0 &&
      wire >= static_cast<double>(std::min(kGoodWireCap, target));

  const int index = index_value(state->current);
  tier candidate = state->current;
  if (bad && target <= plan.floor && index > 0) {
    candidate = tier_at(index - 1);
  } else if (good && index < tier_count - 1 &&
             value.bandwidth_bps < kTierUpBandwidthLimitBps) {
    candidate = tier_at(index + 1);
  }

  if (candidate == state->current) {
    state->has_candidate = false;
    state->streak = 0;
    return result;
  }
  if (state->has_candidate && state->candidate == candidate) {
    ++state->streak;
  } else {
    state->has_candidate = true;
    state->candidate = candidate;
    state->streak = 1;
  }

  // Slow, asymmetric hysteresis: three bad samples to step down, five good
  // ones to step back up, with a long dwell so a marginal link cannot make
  // the picture change every few seconds.
  const bool downgrade =
      index_value(candidate) < index_value(state->current);
  const int needed = downgrade ? kTierDownSamples : kTierUpSamples;
  const uint64_t dwell = downgrade ? kTierDownDwellMs : kTierUpDwellMs;
  if (state->streak >= needed && now_ms - state->last_change_ms >= dwell) {
    state->last_change_ms = now_ms;
    state->current = candidate;
    state->has_candidate = false;
    state->streak = 0;
    result.changed = true;
    result.value = candidate;
  }
  return result;
}

fps_step step_fps(fps_state* state, const sample& value, const fps_plan& plan,
                  uint64_t now_ms) {
  fps_step result;
  if (state == nullptr) return result;
  const int target = state->target;
  result.target = target;

  const double interval_ms = 1000.0 / std::max(1, target);
  // 85 % of the frame budget; the rest covers grabbing and sending.
  const int cpu_ceiling =
      value.host_build_ms > 0.0
          ? std::max(min_fps,
                     static_cast<int>(std::floor(850.0 / value.host_build_ms)))
          : max_fps;
  const bool latency_bad =
      value.latency_known && value.latency_ms > kFpsLatencyBadMs;
  const bool drop_bad = value.dropped > kFpsDroppedBad;
  const bool underrun =
      value.wire_fps > 0.0 &&
      value.wire_fps <
          std::max(3.0, static_cast<double>(target) * 0.6);
  const bool cpu_bad = cpu_ceiling < target;
  const bool bad = latency_bad || drop_bad || underrun || cpu_bad;
  // Matching the ceiling used above, so a rate that exactly fits the encoder
  // counts as healthy instead of oscillating around it.
  const bool good =
      !bad && (!value.latency_known || value.latency_ms < kFpsGoodLatencyMs) &&
      (value.host_build_ms <= 0.0 ||
       value.host_build_ms <= kBuildBudgetFraction * interval_ms);

  if (bad) {
    state->good = 0;
    ++state->bad;
    if (state->bad < kFpsBadSamples ||
        now_ms - state->last_change_ms < kFpsDownDwellMs) {
      return result;
    }
    int next = std::max(plan.min, static_cast<int>(std::floor(target * 0.75)));
    // A link that only answers a fraction of the pulls has already told us
    // its ceiling; jumping straight there avoids a long staircase.
    if (underrun && value.wire_fps > 0.0) {
      next = std::min(next,
                      std::max(plan.min, static_cast<int>(value.wire_fps)));
    }
    if (cpu_bad) next = std::min(next, cpu_ceiling);
    next = std::max(plan.min, std::min(target - 1, next));
    if (next >= target) return result;
    state->target = next;
    state->bad = 0;
    state->last_change_ms = now_ms;
    result.changed = true;
    result.target = next;
    return result;
  }
  if (good) {
    state->bad = 0;
    ++state->good;
    if (state->good < kFpsGoodSamples || target >= plan.max ||
        now_ms - state->last_change_ms < kFpsUpDwellMs) {
      return result;
    }
    // Never recover past what the encoder can actually sustain.
    const int step = target < 20 ? 5 : 10;
    const int next = std::min(std::min(plan.max, target + step), cpu_ceiling);
    if (next <= target) return result;
    state->target = next;
    state->good = 0;
    state->last_change_ms = now_ms;
    result.changed = true;
    result.target = next;
    return result;
  }
  state->good = 0;
  state->bad = 0;
  return result;
}

void reset_auto(tier_state* tiers, fps_state* fps, uint64_t now_ms) {
  if (tiers != nullptr) {
    tiers->current = tier::balanced;
    tiers->candidate = tier::balanced;
    tiers->has_candidate = false;
    tiers->streak = 0;
    tiers->last_change_ms = now_ms;
  }
  if (fps != nullptr) {
    fps->target = plan_for(tier::balanced).start;
    fps->good = 0;
    fps->bad = 0;
    fps->last_change_ms = now_ms;
  }
}

}  // namespace molink::adaptive
