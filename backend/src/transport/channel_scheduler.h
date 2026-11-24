#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "transport/session_transport.h"

namespace molink {

/// Outcome of offering one video frame to a transport.
enum class video_send_status {
  /// The transport accepted the frame.
  sent,
  /// The transport is alive but its queue is full. This is local backpressure:
  /// the frame is dropped silently and the reference chain is rebuilt.
  dropped,
  /// The transport is gone or the payload could not be sent at all.
  failed,
};

/// Rate-limits the recovery actions taken when the video queue refuses a
/// frame. A keyframe costs a full-screen encode and the peer notice rides the
/// control channel, so one burst of dropped frames asks at most once per
/// interval.
class video_drop_throttle final {
 public:
  static constexpr uint64_t interval_ms = 500;

  /// Returns true when a recovery action is due, and opens the interval.
  bool due(uint64_t now_ms);
  void reset();

 private:
  uint64_t last_ms_ = 0;
  bool armed_ = false;
};

/// Applies the video backpressure policy after `accepted` refused a frame.
///
/// Backpressure on a live transport is expected and recoverable: no error is
/// reported to the peer, and the keyframe request plus `stream-gap` notice are
/// fire-and-forget and throttled. A closed transport is a genuine send failure
/// and keeps reporting the error.
video_send_status handle_video_send_result(
    bool accepted, bool transport_open, uint64_t now_ms,
    video_drop_throttle& throttle,
    const std::function<void()>& request_keyframe,
    const std::function<void()>& notify_gap,
    const std::function<void()>& report_error);

/// A bounded, single-threaded egress scheduler for session channels.
///
/// The scheduler owns queued bytes and never blocks the caller. Control,
/// audio, and file messages are rejected when their channel budget is full.
/// Video is newest-first: when the queue is over budget the oldest deltas are
/// discarded until the new frame fits, because a queued old frame is latency
/// the viewer cannot get back. A new keyframe supersedes all queued video.
/// Every discard is counted so the caller can request a fresh baseline and
/// report it; one frame is always accepted, even when it alone exceeds the
/// budget.
class channel_scheduler final {
 public:
  struct limits {
    /// One clipboard-sync message carries up to 192 KiB of text plus JSON
    /// framing, and it is a single reliable message: the control budget has to
    /// stay above that or a large copy silently stops syncing.
    std::size_t control_bytes = 256 * 1024;
    std::size_t video_bytes = 4 * 1024 * 1024;
    std::size_t audio_bytes = 256 * 1024;
    std::size_t file_bytes = 4 * 1024 * 1024;
  };

  struct message {
    transport_channel channel = transport_channel::control;
    std::vector<uint8_t> data;
    bool keyframe = false;
    bool text = false;
  };

  channel_scheduler();
  explicit channel_scheduler(limits configured);

  /// Enqueue a message. Returns false at the channel's byte budget. A single
  /// video frame can exceed that budget when the video queue is empty.
  bool enqueue(transport_channel channel, const uint8_t* data, std::size_t len,
               bool keyframe = false);
  bool enqueue_text(const std::string& text);

  /// Sends at most one message per channel in priority order. The callback
  /// returns false when the underlying transport applies backpressure; the
  /// message remains queued for a later drain.
  std::size_t drain(const std::function<bool(const message&)>& send,
                    std::size_t max_messages = static_cast<std::size_t>(-1));

  std::size_t queued_bytes(transport_channel channel) const;
  std::size_t queued_messages(transport_channel channel) const;
  bool empty() const;

  /// Video frames discarded by the budget policy since the last take. A drop
  /// breaks the reference chain, so the caller must ask the capture for a new
  /// baseline; the count also feeds the pipeline's `dropped` summary.
  std::size_t dropped_video_frames() const { return dropped_video_; }
  std::size_t take_dropped_video_frames();

 private:
  struct queue {
    std::deque<message> messages;
    std::size_t bytes = 0;
    std::size_t limit = 0;
  };

  queue& queue_for(transport_channel channel);
  const queue& queue_for(transport_channel channel) const;
  bool evict_video_for(std::size_t required, bool incoming_keyframe);

  queue control_;
  queue video_;
  queue audio_;
  queue file_;
  std::size_t dropped_video_ = 0;
};

}  // namespace molink
