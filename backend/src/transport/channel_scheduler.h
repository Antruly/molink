#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "transport/session_transport.h"

namespace molink {

/// A bounded, single-threaded egress scheduler for session channels.
///
/// The scheduler owns queued bytes and never blocks the caller. Control,
/// audio, and file messages are rejected when their channel budget is full.
/// Video is latency-sensitive: stale non-keyframes are discarded to make room
/// for a newer frame, while queued keyframes are retained.
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

  /// Enqueue a message. Returns false when a reliable channel is over budget
  /// or when a video keyframe cannot fit without evicting another keyframe.
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
};

}  // namespace molink
