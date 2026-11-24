#pragma once

#include <cstddef>

#include "transport/session_transport.h"

namespace molink {

/// What a bounded TCP write queue does with one packet.
enum class tcp_backlog_action {
  /// There is room; queue and write it.
  accept,
  /// The queue is full and this is a video frame. The frame is obsolete by the
  /// time the backlog drains, so drop it and keep the link up. The caller
  /// treats the refusal as local backpressure and asks for a keyframe.
  drop_video,
  /// The queue is full and this packet is not video (control, audio, file).
  /// Those are reliable: if even they cannot be queued the peer is not reading
  /// at all, which is a genuine link failure.
  fail_link,
};

/// Admits one packet into a queue holding @p queued bytes, bounded by
/// @p limit. Video is the only channel that may be discarded: dropping a
/// control or file message would corrupt the session rather than delay it.
inline tcp_backlog_action tcp_backlog_admit(transport_channel channel,
                                            std::size_t queued,
                                            std::size_t len,
                                            std::size_t limit) {
  if (queued + len <= limit) return tcp_backlog_action::accept;
  return channel == transport_channel::video ? tcp_backlog_action::drop_video
                                             : tcp_backlog_action::fail_link;
}

}  // namespace molink
