#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace molink {

/// Milliseconds on a monotonic clock. The AV1 budget policy only compares
/// intervals, so a wall clock would bring NTP steps into the decision.
inline uint64_t monotonic_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

/// The frame-time AV1 quality trade is bounded: at most this many QP steps
/// above the preset, and never coarser than kMaxAv1Qp (Q 48 is blocky enough
/// that a lower frame rate is the better trade).
inline constexpr int kMaxAv1QpOffset = 20;
inline constexpr int kMaxAv1Qp = 48;

/// The time one frame may spend being grabbed and encoded: 85 % of a frame at
/// `fps`, leaving the rest for grabbing and sending.
inline uint64_t frame_budget_us(int fps) {
  if (fps < 1) fps = 1;
  return static_cast<uint64_t>(1000000) * 85 /
         (100ull * static_cast<uint64_t>(fps));
}

/// Steps the AV1 QP offset after one frame, given the smoothed build time.
///
/// A CPU-only host that cannot encode a sharp rectangle inside the budget
/// settles at a handful of frames per second: the picture is sharp but visibly
/// stutters. Raising the QP trades some sharpness for movement. The step is
/// larger while the overrun is severe, and quality is reclaimed one step at a
/// time and only when the frame is well inside the budget, so the offset does
/// not oscillate around the boundary.
inline int adapt_av1_qp_offset(int offset, uint64_t build_us,
                               uint64_t budget_us) {
  offset = std::clamp(offset, 0, kMaxAv1QpOffset);
  if (budget_us == 0) return offset;
  if (build_us > budget_us) {
    const int step = build_us > budget_us + budget_us / 3 ? 2 : 1;
    return (std::min)(offset + step, kMaxAv1QpOffset);
  }
  if (build_us * 2 < budget_us) return (std::max)(offset - 1, 0);
  return offset;
}

/// Decides when AV1 has to be abandoned for a while.
///
/// Raising the QP is not always enough: a CPU-only host encoding 1080p intra
/// frames can be several times too slow even at the coarsest QP, and then the
/// session runs at 3-5 fps. Tiles (BGRA + zlib) are lossless but far cheaper to
/// produce, so the picture moves again at the cost of wire bytes. The switch is
/// deliberately sticky: a session that has proven too slow is retried only
/// after a cooldown, and the cooldown doubles each time the retry fails, so the
/// codec cannot flap between AV1 and tiles every few seconds. A retry that
/// stays inside the budget for kSettledMs restores the short cooldown.
struct av1_budget_policy {
  /// Consecutive over-budget frames, with the QP already at its bound, before
  /// AV1 is dropped. One slow keyframe must not decide the session.
  static constexpr int kOverBudgetStreak = 3;
  static constexpr uint64_t kInitialCooldownMs = 20000;
  static constexpr uint64_t kMaxCooldownMs = 300000;
  static constexpr uint64_t kSettledMs = 10000;

  bool suspended = false;
  uint64_t suspend_until_ms = 0;
  uint64_t cooldown_ms = kInitialCooldownMs;
  uint64_t trial_started_ms = 0;
  int over_budget_streak = 0;

  /// Called after each frame built while AV1 is in play. Returns true on the
  /// frame that gives up on AV1.
  bool note_frame(bool over_budget, bool qp_maxed, uint64_t now_ms) {
    if (suspended) return false;
    if (trial_started_ms != 0 && !over_budget &&
        now_ms - trial_started_ms >= kSettledMs) {
      // The retry held: forget the backoff.
      cooldown_ms = kInitialCooldownMs;
      trial_started_ms = 0;
    }
    if (!over_budget || !qp_maxed) {
      over_budget_streak = 0;
      return false;
    }
    if (++over_budget_streak < kOverBudgetStreak) return false;
    over_budget_streak = 0;
    suspended = true;
    suspend_until_ms = now_ms + cooldown_ms;
    trial_started_ms = 0;
    cooldown_ms = (std::min)(cooldown_ms * 2, kMaxCooldownMs);
    return true;
  }

  /// Whether AV1 may encode this frame. `resumed` reports that a suspension
  /// just ended, so the caller can start the retry at the coarsest QP.
  bool allow_av1(uint64_t now_ms, bool& resumed) {
    resumed = false;
    if (!suspended) return true;
    if (now_ms < suspend_until_ms) return false;
    suspended = false;
    trial_started_ms = now_ms;
    resumed = true;
    return true;
  }

  void reset() { *this = av1_budget_policy{}; }
};

}  // namespace molink

