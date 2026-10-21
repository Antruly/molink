#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace molink {

/// One captured screen update, already encoded as a FRAME payload.
///
/// The payload is built on the capture thread so the event loop only has to
/// prepend a packet header. `keyframe` means every tile is present; otherwise
/// only tiles that changed since the previous frame are included.
struct captured_frame {
  uint32_t seq = 0;
  uint64_t ts_us = 0;
  bool keyframe = false;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t tile_w = 0;
  uint16_t tile_h = 0;
  uint8_t codec = 0;  ///< protocol::codec of the payload
  std::vector<uint8_t> payload;
};

struct capture_stats {
  uint64_t frames = 0;
  uint64_t keyframes = 0;
  uint64_t full_grabs = 0;
  uint64_t rect_grabs = 0;
  uint64_t bytes = 0;
  uint16_t last_dirty_w = 0;
  uint16_t last_dirty_h = 0;
};

struct capture_options {
  int fps = 15;
  uint16_t tile_size = 256;
  /// Compress tiles with zlib when the frame is compressible.
  bool compress = true;
  // Synthetic source dimensions (also the fallback when no display is
  // reachable). The X11 source uses the real screen size instead.
  uint16_t synthetic_width = 640;
  uint16_t synthetic_height = 360;
};

class capture_source {
 public:
  virtual ~capture_source() = default;

  virtual bool start(std::string& err) = 0;
  virtual void stop() = 0;
  virtual bool running() const = 0;

  /// Short identifier: "x11-shm", "x11", "synthetic".
  virtual std::string backend() const = 0;

  /// Most recent encoded frame, or nullptr before the first one.
  virtual std::shared_ptr<const captured_frame> latest() const = 0;

  /// Frame with exactly this capture sequence, or nullptr once it has been
  /// evicted from the history. Clients resuming after a stall replay from here
  /// instead of forcing a new keyframe.
  virtual std::shared_ptr<const captured_frame> find(uint32_t seq) const = 0;

  /// Ask the source to mark every tile in the next frame. Used when a client
  /// has no base image yet, or missed a frame.
  virtual void request_keyframe() = 0;

  /// Capture only while at least one client is watching. A static desktop with
  /// no viewers should not cost a core.
  virtual void set_active(bool active) = 0;

  /// True when frames come from a real display rather than the test pattern.
  virtual bool is_real() const = 0;

  virtual capture_stats stats() const = 0;
};

/// Shared bookkeeping for concrete sources: latest-frame slot, keyframe
/// request flag, running flag.
class capture_base : public capture_source {
 public:
  std::shared_ptr<const captured_frame> latest() const override;
  std::shared_ptr<const captured_frame> find(uint32_t seq) const override;
  void request_keyframe() override { keyframe_requested_.store(true); }
  void set_active(bool active) override { active_.store(active); }
  bool running() const override { return running_.load(); }
  capture_stats stats() const override;

  /// Hard bounds for the replay history, whichever comes first. Partial frames
  /// are usually small; the byte cap exists so a burst of full-screen changes
  /// cannot grow memory without limit.
  static constexpr std::size_t kHistoryMaxBytes = 128u * 1024u * 1024u;
  static constexpr std::size_t kHistoryMaxFrames = 120;

 protected:
  void publish(std::shared_ptr<const captured_frame> frame);
  void note_grab(bool full, uint16_t dirty_w, uint16_t dirty_h);

  mutable std::mutex mutex_;
  std::shared_ptr<const captured_frame> latest_;
  std::deque<std::shared_ptr<const captured_frame>> history_;
  std::size_t history_bytes_ = 0;
  std::atomic<bool> keyframe_requested_{false};
  std::atomic<bool> active_{false};
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> stat_frames_{0};
  std::atomic<uint64_t> stat_keyframes_{0};
  std::atomic<uint64_t> stat_full_grabs_{0};
  std::atomic<uint64_t> stat_rect_grabs_{0};
  std::atomic<uint64_t> stat_bytes_{0};
  std::atomic<uint16_t> stat_dirty_w_{0};
  std::atomic<uint16_t> stat_dirty_h_{0};
};

/// Returns nullptr when the build has no X11 support.
std::unique_ptr<capture_source> make_x11_capture(const capture_options& options);
std::unique_ptr<capture_source> make_synthetic_capture(const capture_options& options);

/// Starts a real display source when possible, otherwise the synthetic one.
/// `note` always receives a human-readable description for logs and /api/info.
std::shared_ptr<capture_source> start_capture(const capture_options& options,
                                              bool allow_display,
                                              std::string& note);

}  // namespace molink
