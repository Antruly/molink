#include "transport/video_send.h"

#include <utility>

#include "protocol/frame_packet.h"

namespace molink {

video_packet_result send_frame_packet(
    const std::shared_ptr<session_info>& session, uint16_t flags,
    uint64_t ts_us, const std::vector<uint8_t>& payload) {
  video_packet_result result;
  if (!session) return result;

  // Lock first. This is the only reference the send path is allowed to use;
  // no raw transport pointer survives a teardown.
  std::shared_ptr<session_transport> transport = session->transport.lock();
  if (!transport) {
    result.status = video_packet_status::no_transport;
    return result;
  }

  std::vector<uint8_t> bytes;
  const uint32_t seq = session->out_seq++;
  if (!protocol::encode_packet_into(
          bytes, static_cast<uint8_t>(protocol::packet_type::frame), flags,
          session->id, seq, ts_us, payload.data(), payload.size(),
          result.error)) {
    result.status = video_packet_status::encode_failed;
    return result;
  }

  if (transport->send_binary(transport_channel::video, bytes.data(),
                             bytes.size())) {
    result.status = video_packet_status::sent;
    ++session->frames_sent;
    return result;
  }

  result.status = transport->is_open() ? video_packet_status::refused_open
                                       : video_packet_status::refused_closed;
  return result;
}

}  // namespace molink
