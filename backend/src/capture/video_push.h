#pragma once

#include <algorithm>
#include <cstdint>

namespace molink {

/// Fixed send clock for a session that asked the host to push video.
///
/// The pull model answers every `request-frame`: the client sets the cadence
/// and pays a round trip per frame. For a moving picture that is the wrong
/// shape -- the host already has a newer frame while the request travels, and
/// a stalled client keeps asking for frames it cannot keep up with.
///
/// This clock is the host-side replacement: `due()` fires at the capture rate,
/// so the sender always offers the newest capture and stale intermediate
/// frames are simply never sent. `note_viewer()` closes the loop -- a viewer
/// that reports painting well below the clock halves it, and gets it back
/// once it keeps up again.
///
/// Pure and time-injected on purpose: the scheduling decision is the part
/// worth testing without a socket.
class video_push_clock {
 public:
  static constexpr int kMinFps = 5;
  static constexpr int kDefaultFps = 30;

  explicit video_push_clock(int target_fps = kDefaultFps)
      : base_fps_(clamp_fps(target_fps)), fps_(base_fps_) {}

  /// Ceiling for the clock (the capture rate). A viewer that reported being
  /// behind keeps its lower rate; otherwise the clock follows the ceiling.
  void set_target_fps(int target_fps) {
    base_fps_ = clamp_fps(target_fps);
    fps_ = limited_ ? std::min(fps_, base_fps_) : base_fps_;
  }

  int target_fps() const { return base_fps_; }
  int fps() const { return fps_; }

  /// True when a frame is due at @p now_ms. A due call advances the clock, so
  /// a skipped send (nothing new to show, or a refused transport) does not
  /// make the next frame late: the cadence stays fixed rather than drifting.
  bool due(uint64_t now_ms) {
    if (!started_) {
      started_ = true;
      last_ms_ = now_ms;
      return true;
    }
    const uint64_t interval = interval_ms();
    if (now_ms >= last_ms_ && now_ms - last_ms_ < interval) return false;
    last_ms_ = now_ms;
    return true;
  }

  /// Viewer feedback, at most once a second. Painting well below the clock
  /// with drops means the viewer, not the codec, is the limit: halve the
  /// rate. A viewer that keeps up recovers one step at a time, so a single
  /// good second does not immediately overload it again.
  void note_viewer(double paint_fps, int dropped, uint64_t now_ms) {
    if (last_viewer_ms_ != 0 && now_ms - last_viewer_ms_ < 1000) return;
    last_viewer_ms_ = now_ms;
    if (paint_fps <= 0.0) return;
    if (paint_fps < fps_ * 0.6 && dropped > 0) {
      limited_ = true;
      fps_ = std::max(kMinFps, fps_ / 2);
    } else if (paint_fps >= fps_ * 0.9 && fps_ < base_fps_) {
      fps_ = std::min(base_fps_, fps_ + std::max(1, base_fps_ / 4));
      if (fps_ >= base_fps_) limited_ = false;
    }
  }

  /// Milliseconds between frames at the current rate.
  uint64_t interval_ms() const {
    return static_cast<uint64_t>(1000 / std::max(1, fps_));
  }

 private:
  static int clamp_fps(int fps) {
    return std::clamp(fps, kMinFps, 120);
  }

  int base_fps_;
  int fps_;
  /// A viewer pulled the clock below the capture rate; only that viewer's
  /// recovery may put it back, not an unrelated settings change.
  bool limited_ = false;
  bool started_ = false;
  uint64_t last_ms_ = 0;
  uint64_t last_viewer_ms_ = 0;
};

}  // namespace molink
