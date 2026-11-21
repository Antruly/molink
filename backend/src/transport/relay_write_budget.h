#pragma once

#include <cstddef>

#include "protocol/frame_packet.h"
#include "transport/session_transport.h"

namespace molink {

// Leave space for control/audio on a shared TCP write queue. A legal video
// frame larger than the normal window is allowed only with no writes pending;
// otherwise a large lossless baseline would remain at the queue head forever.
inline bool relay_write_fits(transport_channel channel, std::size_t size,
                             std::size_t inflight) {
  constexpr std::size_t window = 1024 * 1024;
  constexpr std::size_t reserved = 256 * 1024;
  constexpr std::size_t max_packet =
      protocol::kMaxPayloadBytes + protocol::kPacketHeaderSize;
  if (size > max_packet) return false;
  const bool latency_sensitive = channel == transport_channel::control ||
                                 channel == transport_channel::audio;
  const auto limit = latency_sensitive ? window : window - reserved;
  if (size > limit) {
    return channel == transport_channel::video && inflight == 0;
  }
  return inflight <= limit && size <= limit - inflight;
}

}  // namespace molink
