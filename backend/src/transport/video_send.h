#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "session_registry.h"

namespace molink {

/// Outcome of offering one encoded frame packet to the session's transport.
enum class video_packet_status {
  /// The transport accepted the packet.
  sent,
  /// The session's transport is closed or already gone. Nothing was encoded
  /// and nothing was sent.
  no_transport,
  /// The packet could not be built; @ref video_packet_result::error says why.
  encode_failed,
  /// The transport is alive but refused the packet (local backpressure).
  refused_open,
  /// The transport is no longer open and refused the packet.
  refused_closed,
};

struct video_packet_result {
  video_packet_status status = video_packet_status::no_transport;
  std::string error;  ///< Only set for encode_failed.
};

/// Encodes one MOLK FRAME packet and offers it on the session's video channel.
///
/// The transport is reached **only** through `session_info::transport`, which
/// is a weak reference owned by the controller. It is locked before the
/// sequence number is advanced or any bytes are built: a session whose
/// transport has been closed or torn down therefore skips the frame instead of
/// touching a connection that libuvcpp may already have recycled.
///
/// This is the transport-facing half of `send_frame_payload()` in the session
/// controller (which adds the throttled diagnostics and error replies). It is
/// deliberately free of diag/JSON dependencies so the lifetime rule can be
/// unit-tested without a network or a live event loop.
video_packet_result send_frame_packet(
    const std::shared_ptr<session_info>& session, uint16_t flags,
    uint64_t ts_us, const std::vector<uint8_t>& payload);

}  // namespace molink
