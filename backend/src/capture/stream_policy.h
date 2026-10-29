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
///  * enter the stream after at least `kEnterFraction` of the screen has been
///    moving for `kEnterDwellMs` (a video, a scroll, a drag);
///  * leave it once less than that has moved for `kExitDwellMs` (the picture
///    settled);
///  * `kArmFraction` is the early warning: the caller can create the stream
///    encoder before the threshold is crossed, so the switch does not pay the
///    encoder start-up.
///
/// Fractions are 0..1 of the screen area that changed in one capture frame.
struct stream_policy {
  static constexpr double kEnterFraction = 0.10;
  static constexpr double kExitFraction = 0.10;
  static constexpr double kArmFraction = 0.08;
  static constexpr uint64_t kEnterDwellMs = 1000;
  static constexpr uint64_t kExitDwellMs = 10000;

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
    armed = moving_fraction >= kArmFraction;
    if (!streaming) {
      if (moving_fraction < kEnterFraction) {
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
    if (moving_fraction >= kExitFraction) {
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

  void reset() { *this = stream_policy{}; }
};

/// Which codec the low-delay stream should use for the current viewer set.
enum class stream_choice : uint8_t {
  none,
  av1,
  h264,
  vp9,
  vp8,
};

/// AV1 wins whenever every viewer can decode it and the host allows it: it
/// compresses far better. H.264 is the fallback for a viewer set that cannot
/// take AV1 (the browser decodes it through WebCodecs), and when any viewer
/// can decode neither, the capture stays on tiles.
/// The rungs a viewer set can all decode, best first, with the hardware codecs
/// (AV1, H.264) ahead of the CPU ones (VP9, VP8). `av1_enabled` is the host
/// switch: disabling AV1 in the settings drops that rung even when everyone
/// can decode it.
inline stream_choice choose_video_codec(bool av1_enabled, bool all_av1,
                                         bool all_h264, bool all_vp9 = false,
                                         bool all_vp8 = false) {
  if (av1_enabled && all_av1) return stream_choice::av1;
  if (all_h264) return stream_choice::h264;
  if (all_vp9) return stream_choice::vp9;
  if (all_vp8) return stream_choice::vp8;
  return stream_choice::none;
}

}  // namespace molink
