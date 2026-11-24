#pragma once

#include <algorithm>
#include <cstdint>

namespace molink {

/// Weight-based quality controller for the low-delay stream.
///
/// The toolbar sliders are *weights*, not absolute values: the user says how
/// much quality/bitrate/frame rate they would like, and this controller moves
/// the encoder around those anchors while the link and the viewers decide what
/// is actually affordable. Its priority order is the one the product wants:
///
///   1. never stutter (frame rate must survive),
///   2. keep the frame rate steady,
///   3. only then spend the remaining budget on quality.
///
/// It is fed one window per second and produces a multiplicative bitrate scale
/// and a small QP offset, plus the constant-quality/constant-bitrate decision.
///
/// The controller is deliberately damped, because the previous one was not:
///
///  * a bad window counts only when it is followed by another bad window
///    (two consecutive windows), and the scale moves one step at a time
///    (×0.85) at most once per three seconds;
///  * a healthy window widens the budget by ×1.12, also at most once per three
///    seconds, so recovery is a smooth climb rather than a five-second tick;
///  * queueing delay is the preferred pressure signal; when it is not measured
///    the viewer's dropped-frame count is used instead, still requiring
///    consecutive bad windows;
///  * at the floor the controller stops believing in quality control and
///    switches the encoder to CBR, then coarsens the QP in steps of 2;
///  * a static screen (no demand) never moves anything.
///
/// Every state change is reported through @ref last_change so the caller can
/// write one `quality.qos` line per move instead of every window.
class stream_qos {
 public:
  enum class change {
    none,
    down,
    up,
  };

  struct input {
    double paint_fps = 0;    ///< Slowest viewer over the window.
    int target_fps = 30;     ///< What the user asked for.
    int dropped = 0;         ///< Frames the slowest viewer dropped.
    double rtt_ms = 0;       ///< Worst viewer RTT (0 = unknown).
    /// Worst queueing delay over the window, in ms (capture-done to send).
    /// Zero means "not measured"; `dropped` is then the pressure signal.
    double queue_delay_ms = 0;
    bool demand = false;     ///< The host produced frames in this window.
  };

  struct output {
    double bitrate_scale = 1.0;
    int qp_offset = 0;
    bool force_cbr = false;
  };

  /// Two bad windows in a row are needed before a step down.
  static constexpr int kBadWindowsRequired = 2;
  /// Minimum spacing between two state changes in either direction.
  static constexpr uint64_t kDwellMs = 3000;
  /// One down step. Smaller than the old 0.75 so a single correction does not
  /// overshoot and force the recovery to undo it.
  static constexpr double kDownStep = 0.85;
  /// One recovery step, applied every dwell instead of every five seconds.
  static constexpr double kUpStep = 1.12;
  /// Queueing delay that counts as pressure / as comfortable.
  static constexpr double kQueueDelayBadMs = 150.0;
  static constexpr double kQueueDelayGoodMs = 80.0;

  /// Feeds one window. Returns true when the output changed, so the caller can
  /// push it into the capture without waking it every second.
  bool note(const input& in, uint64_t now_ms) {
    if (!in.demand) {
      // A still screen says nothing about the link: leaving the scale alone is
      // what keeps a paused session from slowly ratcheting up to a level the
      // link cannot carry once movement returns.
      return false;
    }
    const double target = in.target_fps > 0 ? in.target_fps : 30.0;
    const double fps_ratio = in.paint_fps > 0 ? in.paint_fps / target : 1.0;
    const bool queue_known = in.queue_delay_ms > 0.0;
    const bool queue_bad =
        queue_known && in.queue_delay_ms >= kQueueDelayBadMs;
    // `dropped` is the fallback when no queueing delay was measured. It still
    // has to persist for two windows, because one lost frame is normal.
    const bool dropped_bad = !queue_known && in.dropped > 0;
    const bool fps_bad = fps_ratio < 0.7;
    const bool rtt_bad = in.rtt_ms > 200.0;
    const bool bad = queue_bad || dropped_bad || fps_bad || rtt_bad;

    const bool tight =
        !bad && ((queue_known && in.queue_delay_ms >= kQueueDelayGoodMs) ||
                 fps_ratio < 0.9 || in.rtt_ms > 120.0);
    // With a measured queueing delay, viewer drop counts are informational:
    // the delay already says whether the host is the bottleneck. Only when it
    // is unavailable does `dropped` decide a healthy window.
    const bool good = !bad && !tight && (queue_known || in.dropped == 0) &&
                      fps_ratio >= 0.9 && in.rtt_ms <= 120.0;

    const char* reason = queue_bad  ? "queue"
                         : dropped_bad ? "dropped"
                         : fps_bad  ? "paint"
                                    : "rtt";

    change kind = change::none;
    bool changed_state = false;
    const bool dwell_ready =
        !has_changed_ || now_ms - last_change_ms_ >= kDwellMs;

    if (bad) {
      good_streak_ = 0;
      if (bad_streak_ < 1000) ++bad_streak_;
      if (bad_streak_ >= kBadWindowsRequired && dwell_ready) {
        const double before = scale_;
        scale_ = (std::max)(kMinScale, scale_ * kDownStep);
        changed_state = scale_ != before;
        // At the floor, quality control cannot help any more: cap the
        // bitrate, and if even that fails coarsen the QP in small steps.
        if (scale_ <= kMinScale + 1e-9) {
          if (bad_streak_ >= 4 && !force_cbr_) {
            force_cbr_ = true;
            changed_state = true;
          }
          if (force_cbr_ && bad_streak_ >= 6) {
            const int qp = (std::min)(kMaxQpOffset, qp_offset_ + 2);
            if (qp != qp_offset_) {
              qp_offset_ = qp;
              changed_state = true;
            }
          }
        }
        if (changed_state) kind = change::down;
      }
    } else if (good) {
      bad_streak_ = 0;
      if (good_streak_ < 1000) ++good_streak_;
      if (dwell_ready) {
        const double before = scale_;
        scale_ = (std::min)(kMaxScale, scale_ * kUpStep);
        changed_state = scale_ != before;
        if (qp_offset_ > 0) {
          --qp_offset_;
          changed_state = true;
        }
        // A CBR ceiling that is no longer needed wastes detail.
        if (force_cbr_ && scale_ >= kMaxScale - 1e-9) {
          force_cbr_ = false;
          changed_state = true;
        }
        if (changed_state) kind = change::up;
      }
    } else {
      // In between: keep the current state, just let the streaks cool down.
      good_streak_ = 0;
      bad_streak_ = 0;
    }

    if (kind != change::none) {
      last_change_ = kind;
      last_reason_ = kind == change::up ? "healthy" : reason;
      last_dwell_ms_ =
          has_changed_ ? now_ms - last_change_ms_ : now_ms;
      last_change_ms_ = now_ms;
      has_changed_ = true;
    }

    const output next{scale_, qp_offset_, force_cbr_};
    const bool changed = next.bitrate_scale != last_.bitrate_scale ||
                         next.qp_offset != last_.qp_offset ||
                         next.force_cbr != last_.force_cbr;
    last_ = next;
    return changed;
  }

  output state() const { return last_; }
  double bitrate_scale() const { return scale_; }
  int qp_offset() const { return qp_offset_; }
  bool force_cbr() const { return force_cbr_; }
  int bad_streak() const { return bad_streak_; }
  int good_streak() const { return good_streak_; }

  /// The most recent move (none before the first one), why it happened, and
  /// how long the previous state had been held. The caller writes one
  /// `quality.qos` line from these.
  change last_change() const { return last_change_; }
  const char* last_reason() const { return last_reason_; }
  uint64_t last_dwell_ms() const { return last_dwell_ms_; }

  static constexpr double kMinScale = 0.25;
  /// The anchors the user chose are the ceiling: the controller may squeeze
  /// under pressure and spring back, but it never spends more than the slider
  /// says.
  static constexpr double kMaxScale = 1.0;
  static constexpr int kMaxQpOffset = 12;

 private:
  double scale_ = 1.0;
  int qp_offset_ = 0;
  bool force_cbr_ = false;
  int bad_streak_ = 0;
  int good_streak_ = 0;
  output last_{};
  change last_change_ = change::none;
  const char* last_reason_ = "";
  uint64_t last_dwell_ms_ = 0;
  uint64_t last_change_ms_ = 0;
  bool has_changed_ = false;
};

}  // namespace molink
