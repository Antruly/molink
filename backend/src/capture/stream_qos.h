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
/// and a small QP offset, plus the constant-quality/constant-bitrate decision:
///
///  * a bad window (painted fps far below the target, dropped frames, or an
///    RTT well past the interactive threshold) multiplies the budget by 0.75;
///  * at the floor the controller stops believing in quality control and
///    switches the encoder to CBR, which makes the ceiling hard;
///  * if even that is not enough it starts coarsening the QP in steps of 2;
///  * a healthy streak widens the budget by 12 % every five seconds and walks
///    the QP offset back, and once the budget has recovered the mode returns
///    to constant quality.
///
/// Every step has a dwell, so the picture does not oscillate between two
/// states once a second; a static screen (no demand) never moves anything.
class stream_qos {
 public:
  struct input {
    double paint_fps = 0;    ///< Slowest viewer over the window.
    int target_fps = 30;     ///< What the user asked for.
    int dropped = 0;         ///< Frames the slowest viewer dropped.
    double rtt_ms = 0;       ///< Worst viewer RTT (0 = unknown).
    bool demand = false;     ///< The host produced frames in this window.
  };

  struct output {
    double bitrate_scale = 1.0;
    int qp_offset = 0;
    bool force_cbr = false;
  };

  /// Feeds one window. Returns true when the output changed, so the caller can
  /// push it into the capture without waking it every second.
  bool note(const input& in, uint64_t now_ms) {
    (void)now_ms;
    if (!in.demand) {
      // A still screen says nothing about the link: leaving the scale alone is
      // what keeps a paused session from slowly ratcheting up to a level the
      // link cannot carry once movement returns.
      return false;
    }
    const double target = in.target_fps > 0 ? in.target_fps : 30.0;
    const double fps_ratio = in.paint_fps > 0 ? in.paint_fps / target : 1.0;
    const bool bad = fps_ratio < 0.7 || in.dropped > 0 || in.rtt_ms > 200.0;
    const bool tight = fps_ratio < 0.85 || in.rtt_ms > 150.0;
    const bool good =
        fps_ratio >= 0.9 && in.dropped == 0 && in.rtt_ms <= 120.0;

    if (bad) {
      good_streak_ = 0;
      ++bad_streak_;
      scale_ = (std::max)(kMinScale, scale_ * 0.75);
      // At the floor, quality control cannot help any more: cap the bitrate.
      if (scale_ <= kMinScale + 1e-9 && bad_streak_ >= 4) force_cbr_ = true;
      // Still failing with a hard ceiling: coarsen in small steps.
      if (force_cbr_ && bad_streak_ >= 8) {
        qp_offset_ = (std::min)(kMaxQpOffset, qp_offset_ + 2);
      }
    } else if (tight) {
      good_streak_ = 0;
      bad_streak_ = 0;
      // Not yet a stall: shave the budget a little before the queue grows.
      scale_ = (std::max)(kMinScale, scale_ * 0.9);
    } else if (good) {
      bad_streak_ = 0;
      ++good_streak_;
      // Five healthy seconds give budget back towards the user's anchor (the
      // scale never exceeds it); ten let constant quality return (a CBR
      // ceiling that is no longer needed wastes detail).
      if (good_streak_ % 5 == 0) {
        scale_ = (std::min)(kMaxScale, scale_ * 1.12);
        if (qp_offset_ > 0) --qp_offset_;
      }
      if (force_cbr_ && good_streak_ >= 10 && scale_ >= 0.999) {
        force_cbr_ = false;
      }
    } else {
      // In between: keep the current state, just let the streaks cool down.
      good_streak_ = 0;
      bad_streak_ = 0;
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
};

}  // namespace molink
