#pragma once

#include <algorithm>
#include <cstdint>

namespace molink {

/// When to switch between the tile/patch path and a real AV1 video stream.
///
/// The stream path (inter-frame prediction) is dramatically cheaper for moving
/// pictures -- a P frame only codes what changed -- but it forces full-screen
/// frames and a stateful decoder, and it is pointless for a still screen where
/// the patch path sends almost nothing. So the mode follows the content:
///
///  * enter the stream after at least `enter_fraction` of the screen has been
///    moving for `kEnterDwellMs` (a video, a scroll, a drag);
///  * leave it once less than that has moved for `kExitDwellMs` (the picture
///    settled);
///  * `arm_fraction` is the early warning: the caller can create the stream
///    encoder before the threshold is crossed, so the switch does not pay the
///    encoder start-up.
///
/// Fractions are 0..1 of the screen area that changed in one capture frame.
struct stream_policy {
  static constexpr double kDefaultEnterFraction = 0.15;
  static constexpr uint64_t kEnterDwellMs = 1000;
  static constexpr uint64_t kExitDwellMs = 10000;

  /// How much of the screen must move before the stream takes over, and how
  /// little may move before it hands back to tiles. Both come from the host
  /// setting (default 15 %); the exit threshold follows the enter threshold.
  double enter_fraction = kDefaultEnterFraction;
  double exit_fraction = kDefaultEnterFraction;
  /// Early warning so the encoder can be built before the threshold is hit.
  double arm_fraction = kDefaultEnterFraction * 0.8;

  /// Applies a new enter threshold (0..1). The latches are left alone: a
  /// running stream keeps running until the exit dwell expires.
  void set_enter_fraction(double fraction) {
    enter_fraction = std::clamp(fraction, 0.01, 1.0);
    exit_fraction = enter_fraction;
    arm_fraction = (std::max)(0.01, enter_fraction * 0.8);
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
  bool note(double moving_fraction, uint64_t now_ms) {
    armed = moving_fraction >= arm_fraction;
    if (!streaming) {
      if (moving_fraction < enter_fraction) {
        entering = false;
        return false;
      }
      if (!entering) {
        entering = true;
        enter_since_ms = now_ms;
        return false;
      }
      if (now_ms - enter_since_ms < kEnterDwellMs) return false;
      streaming = true;
      entering = false;
      exiting = false;
      return true;
    }
    if (moving_fraction >= exit_fraction) {
      exiting = false;
      return false;
    }
    if (!exiting) {
      exiting = true;
      exit_since_ms = now_ms;
      return false;
    }
    if (now_ms - exit_since_ms < kExitDwellMs) return false;
    streaming = false;
    exiting = false;
    return true;
  }

  void reset() {
    // Keep the configured thresholds; only the latches restart.
    const double enter = enter_fraction;
    *this = stream_policy{};
    set_enter_fraction(enter);
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
