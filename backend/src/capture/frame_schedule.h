#pragma once

#include <cstdint>

namespace molink {

/// Absolute-deadline pacing for a capture loop.
///
/// Sleeping a full interval *after* every grab makes the period encode time
/// plus interval, so the achieved rate is systematically below the target and
/// the shortfall grows with the encoder's cost. This keeps a deadline grid
/// instead: each completed capture advances the deadline by exactly one
/// interval, so the long-run rate follows the target.
///
/// If a grab overran by more than one whole interval, the missed slots are not
/// replayed: the deadline is reset to "now", which captures the next picture
/// immediately and then continues on a fresh grid. That bounds catch-up to one
/// frame and keeps a slow host from accumulating a burst it can never drain.
///
/// Times are monotonic microseconds; the type is plain int64 so the policy is
/// testable without a clock.
class frame_schedule {
 public:
  using time_us = int64_t;

  /// Start (or restart) the grid: the next capture is one interval from now.
  void reset(time_us now_us, int64_t interval_us) {
    interval_us_ = interval_us > 0 ? interval_us : 1;
    next_us_ = now_us + interval_us_;
    behind_ = false;
  }

  /// Called after a capture completed at @p now_us. Advances the deadline by
  /// one interval, unless more than one interval was lost, in which case the
  /// next capture is due immediately.
  ///
  /// @p reanchor is for a capture that started early on purpose (input made
  /// the caller skip the wait). The grid restarts from that picture; adding
  /// one interval to a still-future deadline would otherwise delay the frame
  /// after it by up to a whole interval.
  void note_capture(time_us now_us, bool reanchor = false) {
    if (reanchor) {
      next_us_ = now_us + interval_us_;
      behind_ = false;
      return;
    }
    next_us_ += interval_us_;
    if (now_us - next_us_ > interval_us_) {
      // More than one whole slot late: do not queue the missed pictures.
      next_us_ = now_us;
      behind_ = true;
    }
  }

  /// A new target rate. Re-anchors the grid so the new spacing applies from
  /// the next slot instead of mixing two intervals on one deadline.
  void set_interval(time_us now_us, int64_t interval_us) {
    interval_us_ = interval_us > 0 ? interval_us : 1;
    next_us_ = now_us + interval_us_;
    behind_ = false;
  }

  bool due(time_us now_us) const { return now_us >= next_us_; }
  time_us next() const { return next_us_; }
  int64_t interval() const { return interval_us_; }

  /// True when the previous capture was late enough that the schedule was
  /// restarted; useful to a caller that wants to report the catch-up.
  bool restarted() const { return behind_; }
  void clear_restarted() { behind_ = false; }

 private:
  int64_t interval_us_ = 1;
  int64_t next_us_ = 0;
  bool behind_ = false;
};

}  // namespace molink
