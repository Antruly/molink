// Viewer-side automatic quality: the tier ladder and the load-aware frame
// rate, without Win32 so Linux unit tests drive the same code the native
// shell runs.
//
// The thresholds, sample counts and dwell times mirror the Web's auto quality
// controller (`frontend/src/components/SessionView.tsx`, "Auto quality" and
// "Average frame rate"): two bad samples are needed to lower the frame rate
// and four good ones to raise it; the image tier needs three bad samples to
// step down and five good ones to step back up, with a 6 s / 10 s dwell.
#pragma once

#include <cstdint>

namespace molink::adaptive {

/// The viewer's fixed picture tiers, weakest first. A8's automatic mode walks
/// this ladder; the wire id for each rung is the `molink::preset` id.
///
/// Parity note: the Web's current `autoTier` walks only smooth/balanced/
/// quality and deliberately never selects turbo (grayscale is jarring) or
/// truecolor (the BGRA tile path). This port follows A8's five-rung ladder;
/// every threshold, sample count and dwell time is still the Web's.
enum class tier : int {
  turbo = 0,
  smooth = 1,
  balanced = 2,
  hd = 3,
  truecolor = 4,
};

constexpr int tier_count = 5;
constexpr int min_fps = 5;
constexpr int max_fps = 60;

/// The auto frame-rate window for one tier. Mirrors the Web's `AUTO_PLANS`
/// after mapping the Web's host modes (smooth/balanced/quality/lossless) to
/// the native preset ids.
struct fps_plan {
  int min = min_fps;
  /// The tier's resting floor: the rate a healthy session works around, and
  /// the point at which the tier ladder starts stepping down.
  int floor = 10;
  int max = 25;
  int start = 15;
};

fps_plan plan_for(tier value);
tier tier_at(int index);
int index_of(tier value);
int clamp_fps(int value);

/// One once-a-second measurement. `latency_known` is false before the first
/// pong: an unknown RTT must not count as healthy (the Web's `latencyMs`
/// null behaves the same way).
struct sample {
  bool latency_known = false;
  int latency_ms = 0;
  double wire_fps = 0.0;
  int dropped = 0;
  /// Bytes per second the session costs on the wire.
  double bandwidth_bps = 0.0;
  int target_fps = 15;
  /// The host's own encode time from `capture-stats`; 0 when unknown.
  double host_build_ms = 0.0;
};

/// Web `autoRef`: the candidate tier and how many samples it has held.
struct tier_state {
  tier current = tier::balanced;
  tier candidate = tier::balanced;
  bool has_candidate = false;
  int streak = 0;
  uint64_t last_change_ms = 0;
};

struct tier_step {
  bool changed = false;
  tier value = tier::balanced;
};

/// Feeds one sample. Returns a change only when the hysteresis and the dwell
/// both allow it.
tier_step step_tier(tier_state* state, const sample& value, uint64_t now_ms);

/// Web `fpsState`: the adapted target and the good/bad run lengths.
struct fps_state {
  int target = 15;
  int good = 0;
  int bad = 0;
  uint64_t last_change_ms = 0;
};

struct fps_step {
  bool changed = false;
  int target = 15;
};

/// Feeds one sample. The plan is the current tier's window; the controller
/// only ever leaves `target` for the host's build time or the link.
fps_step step_fps(fps_state* state, const sample& value, const fps_plan& plan,
                  uint64_t now_ms);

/// Starts a fresh automatic session at the Web's default tier (balanced) and
/// resting rate.
void reset_auto(tier_state* tiers, fps_state* fps, uint64_t now_ms);

/// Web thresholds, exposed so the tests assert the parity and the shell's
/// diagnostics can print the same numbers.
constexpr int kLatencyBadMs = 300;
constexpr int kLatencyGoodMs = 90;
constexpr int kDroppedBad = 5;
constexpr int kGoodWireCap = 12;
constexpr int kTierDownSamples = 3;
constexpr int kTierUpSamples = 5;
constexpr uint64_t kTierDownDwellMs = 6000;
constexpr uint64_t kTierUpDwellMs = 10000;
constexpr double kTierUpBandwidthLimitBps =
    12.0 * 1024.0 * 1024.0;

constexpr int kFpsLatencyBadMs = 220;
constexpr int kFpsDroppedBad = 2;
constexpr int kFpsGoodLatencyMs = 110;
constexpr int kFpsBadSamples = 2;
constexpr int kFpsGoodSamples = 4;
constexpr uint64_t kFpsDownDwellMs = 1500;
constexpr uint64_t kFpsUpDwellMs = 3000;
constexpr double kBuildBudgetFraction = 0.85;

}  // namespace molink::adaptive
