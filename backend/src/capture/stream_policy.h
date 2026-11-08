#pragma once

#include <algorithm>
#include <cstdint>

namespace molink {

/// When to switch between the tile/patch path and a real video stream.
///
/// The stream path (inter-frame prediction) is dramatically cheaper for moving
/// pictures -- a P frame only codes what changed -- but it forces full-screen
/// frames and a stateful decoder, and it is pointless for a still screen where
/// the patch path sends almost nothing. So the mode follows the content.
///
/// The decision is deliberately hysteretic. An earlier version used one
/// threshold for both directions and two short dwells; a drag crossing that
/// threshold back and forth switched tile -> stream -> tile every couple of
/// seconds, and every switch costs a keyframe (and, on CPU encoders, an
/// encoder restart). That ping-pong is what made dragging feel frozen and
/// could push a slow link into a disconnect. The rules now are:
///
///  * movement is averaged over `window_ms` (500 ms), not taken from one frame;
///  * entering needs an average at or above `enter_fraction` for
///    `enter_dwell_ms`;
///  * leaving needs an average at or below `exit_fraction` (60 % of the enter
///    threshold, a real hysteresis band) for `exit_dwell_ms` (2.5 s);
///  * no switch at all within `cooldown_ms` of the previous one;
///  * the caller may veto a switch (`allow_switch` false) while a pointer drag
///    is in flight, the send queue is backed up, or the encoder is blocking.
///
/// Every accepted switch increments `epoch`, the stream_epoch_id the session
/// layer sends to clients so a decoder can drop its old references.
struct stream_policy {
  static constexpr double kDefaultEnterFraction = 0.15;
  static constexpr double kDefaultExitRatio = 0.60;
  static constexpr uint64_t kDefaultWindowMs = 500;
  static constexpr uint64_t kDefaultEnterDwellMs = 500;
  /// Stay on the stream through short pauses: a drag release, a scroll stop.
  /// 2.5 s of a genuinely still picture is the evidence that tiles are cheaper
  /// again; anything shorter produced a stream -> tile -> stream cycle.
  static constexpr uint64_t kDefaultExitDwellMs = 2500;
  /// After either direction, no new switch for this long. The cooldown is a
  /// hard floor even when both dwells are satisfied.
  static constexpr uint64_t kDefaultCooldownMs = 1500;

  /// How much of the screen must move before the stream takes over, and how
  /// little may move before it hands back to tiles. Both come from the host
  /// setting (default 15 %); the exit threshold is `enter_fraction *
  /// exit_ratio`, so it always stays below the enter threshold.
  double enter_fraction = kDefaultEnterFraction;
  double exit_fraction = kDefaultEnterFraction * kDefaultExitRatio;
  /// Early warning so the encoder can be built before the threshold is hit.
  double arm_fraction = kDefaultEnterFraction * 0.8;
  double exit_ratio = kDefaultExitRatio;

  /// Hysteresis timing, in milliseconds.
  uint64_t window_ms = kDefaultWindowMs;
  uint64_t enter_dwell_ms = kDefaultEnterDwellMs;
  uint64_t exit_dwell_ms = kDefaultExitDwellMs;
  uint64_t cooldown_ms = kDefaultCooldownMs;

  /// Applies a new enter threshold (0..1). The latches are left alone: a
  /// running stream keeps running until the exit dwell expires.
  void set_enter_fraction(double fraction) {
    enter_fraction = std::clamp(fraction, 0.01, 1.0);
    exit_fraction = (std::max)(0.01, enter_fraction * exit_ratio);
    arm_fraction = (std::max)(0.01, enter_fraction * 0.8);
  }
  void set_exit_ratio(double ratio) {
    exit_ratio = std::clamp(ratio, 0.10, 0.95);
    exit_fraction = (std::max)(0.01, enter_fraction * exit_ratio);
  }
  void set_timing(uint64_t window, uint64_t enter_dwell, uint64_t exit_dwell,
                  uint64_t cooldown) {
    window_ms = (std::max)(uint64_t{50}, window);
    enter_dwell_ms = enter_dwell;
    exit_dwell_ms = exit_dwell;
    cooldown_ms = cooldown;
  }

  bool streaming = false;
  /// True while the content is close to the enter threshold, so the caller can
  /// prepare the stream encoder.
  bool armed = false;
  bool entering = false;
  bool exiting = false;
  uint64_t enter_since_ms = 0;
  uint64_t exit_since_ms = 0;

  /// Feeds one frame. Returns true on the frame the mode changed, so the caller
  /// requests a keyframe for the path it just switched to.
  ///
  /// @param allow_switch false while a drag is in flight, the send queue is
  ///        backed up, or the encoder is blocking. The latches are dropped, so
  ///        the dwell starts again once the veto is lifted instead of firing
  ///        the instant the queue drains.
  bool note(double moving_fraction, uint64_t now_ms, bool allow_switch = true) {
    push_sample(moving_fraction, now_ms);
    average_ = window_average(now_ms);
    armed = average_ >= arm_fraction;
    if (!allow_switch) {
      entering = false;
      exiting = false;
      return false;
    }
    if (!streaming) {
      if (average_ < enter_fraction) {
        entering = false;
        return false;
      }
      if (!entering) {
        entering = true;
        enter_since_ms = now_ms;
        return false;
      }
      if (now_ms - enter_since_ms < enter_dwell_ms) return false;
      if (in_cooldown(now_ms)) return false;
      stream_switch(true, now_ms);
      return true;
    }
    if (average_ > exit_fraction) {
      exiting = false;
      return false;
    }
    if (!exiting) {
      exiting = true;
      exit_since_ms = now_ms;
      return false;
    }
    if (now_ms - exit_since_ms < exit_dwell_ms) return false;
    if (in_cooldown(now_ms)) return false;
    stream_switch(false, now_ms);
    return true;
  }

  /// Forces the mode (secure desktop, "always stream", an explicit viewer
  /// request). Bumps the epoch exactly like a policy switch.
  bool force(bool on, uint64_t now_ms) {
    if (streaming == on) return false;
    streaming = on;
    entering = false;
    exiting = false;
    ++epoch_;
    last_switch_ms_ = now_ms;
    switched_once_ = true;
    return true;
  }

  /// The switch could not be completed (no encoder for any decodable rung).
  /// Return to tiles and apply the cooldown so the next evaluation does not
  /// retry on every frame.
  void rollback(uint64_t now_ms) {
    if (!streaming) {
      entering = false;
      exiting = false;
      return;
    }
    streaming = false;
    entering = false;
    exiting = false;
    ++epoch_;
    last_switch_ms_ = now_ms;
    switched_once_ = true;
  }

  bool in_cooldown(uint64_t now_ms) const {
    return switched_once_ && now_ms - last_switch_ms_ < cooldown_ms;
  }

  /// Average movement over the last `window_ms` (0..1).
  double average() const { return average_; }
  /// Incremented on every accepted mode switch (stream_epoch_id).
  uint32_t epoch() const { return epoch_; }
  uint64_t last_switch_ms() const { return last_switch_ms_; }

  void reset() {
    // Keep the configured thresholds and timing; only the latches restart.
    const double enter = enter_fraction;
    const double ratio = exit_ratio;
    const uint64_t window = window_ms;
    const uint64_t enter_dwell = enter_dwell_ms;
    const uint64_t exit_dwell = exit_dwell_ms;
    const uint64_t cooldown = cooldown_ms;
    const uint32_t epoch = epoch_;
    *this = stream_policy{};
    epoch_ = epoch;
    set_enter_fraction(enter);
    set_exit_ratio(ratio);
    set_timing(window, enter_dwell, exit_dwell, cooldown);
  }

 private:
  struct movement_sample {
    uint64_t t;
    double value;
  };
  static constexpr int kSampleCapacity = 64;
  movement_sample samples_[kSampleCapacity]{};
  int sample_head_ = 0;
  int sample_count_ = 0;
  double average_ = 0.0;
  uint32_t epoch_ = 0;
  uint64_t last_switch_ms_ = 0;
  bool switched_once_ = false;

  void push_sample(double value, uint64_t now_ms) {
    samples_[sample_head_] = movement_sample{now_ms, value};
    sample_head_ = (sample_head_ + 1) % kSampleCapacity;
    if (sample_count_ < kSampleCapacity) ++sample_count_;
  }

  double window_average(uint64_t now_ms) const {
    double sum = 0.0;
    int count = 0;
    const uint64_t oldest = now_ms > window_ms ? now_ms - window_ms : 0;
    for (int i = 0; i < sample_count_; ++i) {
      const int index = (sample_head_ - 1 - i + kSampleCapacity) %
                        kSampleCapacity;
      const movement_sample& sample = samples_[index];
      if (sample.t < oldest) break;
      sum += sample.value;
      ++count;
    }
    if (count == 0) return 0.0;
    return sum / count;
  }

  void stream_switch(bool on, uint64_t now_ms) {
    streaming = on;
    entering = false;
    exiting = false;
    ++epoch_;
    last_switch_ms_ = now_ms;
    switched_once_ = true;
  }
};

/// Which codec the low-delay stream should use for the current viewer set.
enum class stream_choice : uint8_t {
  none,
  av1,
  h264,
  vp9,
  vp8,
};

/// The rungs a viewer set can all decode, best first. With a hardware AV1
/// encoder that is AV1 > VP9 > VP8 > H.264; the capture stays on tiles when
/// no rung is common to every viewer. `av1_enabled` is the host switch:
/// disabling AV1 in the settings drops that rung even when everyone can
/// decode it.
///
/// `av1_hardware` says whether this host really has an AV1 encoder on the
/// GPU. CPU SVT-AV1 is not in the running for a live desktop: at a few frames
/// per second it looks frozen, so without hardware AV1 the order is
/// VP9 > VP8 > H.264 > AV1 -- compression still beats acceleration among the
/// CPU codecs, but a CPU AV1 stream only wins when nothing else is decodable.
inline stream_choice choose_video_codec(bool av1_enabled, bool all_av1,
                                         bool all_h264, bool all_vp9 = false,
                                         bool all_vp8 = false,
                                         bool av1_hardware = true) {
  if (av1_enabled && all_av1 && av1_hardware) return stream_choice::av1;
  if (all_vp9) return stream_choice::vp9;
  if (all_vp8) return stream_choice::vp8;
  if (all_h264) return stream_choice::h264;
  // Nothing else is shared: a slow stream still beats no stream at all.
  if (av1_enabled && all_av1) return stream_choice::av1;
  return stream_choice::none;
}

}  // namespace molink
