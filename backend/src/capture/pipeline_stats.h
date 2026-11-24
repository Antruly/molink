#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace molink {

/// Why the capture thread produced a frame. Kept small and wire-independent:
/// it exists for the `video.pipeline` diagnostic, not for the protocol.
enum class capture_trigger : uint8_t {
  initial = 0,  ///< startup / first frame after a switch
  timer,        ///< the safety tick fired
  change,       ///< the desktop reported damage (X11) or a new frame (DXGI)
  input,        ///< input arrived and asked for a fresh picture
  keyframe,     ///< a viewer or the periodic refresh asked for a keyframe
  stream,       ///< the low-delay stream keeps a steady cadence
  count,
};

const char* capture_trigger_name(capture_trigger trigger);

/// Epoch microseconds. The diagnostic compares the capture and send clocks,
/// which live on different threads but share the wall clock; both the packet
/// timestamp and the history use this base.
inline uint64_t pipeline_now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

/// The summarised window written as the `video.pipeline` line. Percentiles
/// are nearest-rank over every sample in the window (not an estimate).
struct pipeline_snapshot {
  uint64_t captured = 0;
  uint64_t encoded = 0;
  uint64_t sent = 0;
  uint64_t dropped = 0;
  /// Highest number of captured-but-not-yet-sent frames seen during the
  /// window, across all sessions.
  uint64_t queue_depth = 0;
  uint64_t age_p95_us = 0;
  uint64_t encode_p95_us = 0;
  /// Time from the trigger (damage event, timer deadline, input arrival) to
  /// the start of the grab.
  uint64_t trigger_delay_p95_us = 0;
  /// Time from the last input event to the start of the grab, for captures
  /// that happened while the viewer was active (at most 1 s after input).
  uint64_t input_delay_p95_us = 0;
  uint64_t window_us = 0;
  uint64_t bytes = 0;
  double kbps = 0.0;
  uint64_t triggers[static_cast<std::size_t>(capture_trigger::count)] = {};
};

/// Nearest-rank percentile. `samples` is copied and sorted; pass an empty
/// vector and the result is 0.
uint64_t percentile95(std::vector<uint64_t> samples);

/// Collects one capture/send window. Thread-safe: capture publishes, the
/// session thread records sends and queue depth, and the supervisor takes the
/// summary. The clock is injected so the window boundary and ages are
/// deterministic in tests.
class pipeline_monitor {
 public:
  using clock_fn = std::function<uint64_t()>;

  explicit pipeline_monitor(clock_fn clock = {}, uint64_t window_us = 2000000)
      : clock_(std::move(clock)),
        window_us_(window_us == 0 ? 2000000 : window_us) {}

  /// One published frame: trigger info plus the capture/encode timestamps.
  /// Zero timestamps mean "not measured by this backend" and are skipped.
  void note_capture(capture_trigger trigger, uint64_t trigger_us,
                    uint64_t capture_start_us, uint64_t capture_done_us,
                    uint64_t encode_start_us, uint64_t encode_end_us,
                    uint64_t input_delay_us);
  /// One packet handed to a transport. @p capture_done_us stamps the picture
  /// it carries, so the age is the time the viewer's copy spent in the host.
  void note_sent(uint64_t capture_done_us, uint64_t bytes);
  /// Additional bytes for a picture whose first packet was already counted
  /// (the split tile+AV1 mode).
  void note_sent_bytes(uint64_t bytes);
  /// Frames this session skipped because a newer capture was already there.
  void note_dropped(uint64_t frames);
  /// The newest capture sequence and the sequence this session is up to.
  void note_send_state(uint64_t sent_seq, uint64_t latest_seq);

  /// True when a window boundary has passed at the injected clock.
  bool due() const;
  /// Returns the completed window and starts a new one at the current time.
  pipeline_snapshot take();
  /// The window so far, without resetting it.
  pipeline_snapshot peek() const;

  uint64_t window_us() const { return window_us_; }

  /// Bound on stored samples per metric. A 2 s window holds a few hundred
  /// samples even with several viewers; if a pathological rate exceeds this,
  /// every other sample is discarded, which preserves the distribution.
  static constexpr std::size_t kMaxSamples = 4096;

 private:
  uint64_t now_us() const;
  /// Caller holds mutex_.
  bool due_locked(uint64_t now) const;
  pipeline_snapshot take_locked(uint64_t now);
  static void add_sample(std::vector<uint64_t>& samples, uint64_t value);

  clock_fn clock_;
  uint64_t window_us_;
  mutable std::mutex mutex_;
  uint64_t window_start_us_ = 0;
  bool started_ = false;
  uint64_t captured_ = 0;
  uint64_t encoded_ = 0;
  uint64_t sent_ = 0;
  uint64_t dropped_ = 0;
  uint64_t queue_depth_ = 0;
  uint64_t bytes_ = 0;
  uint64_t triggers_[static_cast<std::size_t>(capture_trigger::count)] = {};
  std::vector<uint64_t> ages_us_;
  std::vector<uint64_t> encode_us_;
  std::vector<uint64_t> trigger_delays_us_;
  std::vector<uint64_t> input_delays_us_;
};

/// The process-wide monitor. Created on first use with the wall clock.
pipeline_monitor& video_pipeline();

}  // namespace molink
