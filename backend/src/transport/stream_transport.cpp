#include "transport/stream_transport.h"

#include <cstring>
#include <utility>

#include "protocol/frame_packet.h"

namespace molink {

stream_transport::stream_transport(write_fn write, close_fn close,
                                   const char* protocol, std::string peer_ip,
                                   unsigned int peer_port)
    : protocol_(protocol != nullptr ? protocol : "stream"),
      peer_ip_(std::move(peer_ip)),
      peer_port_(peer_port),
      write_(std::move(write)),
      close_(std::move(close)) {
  if (peer_ip_.empty()) peer_ip_ = protocol_;
}

stream_transport::~stream_transport() = default;

bool stream_transport::is_open() const {
  return !closed_ && static_cast<bool>(write_);
}

bool stream_transport::send_text(const std::string& text) {
  if (!is_open()) return false;
  protocol::packet packet;
  packet.type = static_cast<uint8_t>(protocol::packet_type::control);
  packet.payload.assign(text.begin(), text.end());
  std::vector<uint8_t> bytes;
  std::string error;
  if (!protocol::encode_packet(packet, bytes, error)) return false;
  return write_(bytes.data(), bytes.size());
}

bool stream_transport::send_binary(const uint8_t* data, std::size_t len) {
  if (!is_open() || (data == nullptr && len > 0)) return false;
  return write_(data, len);
}

void stream_transport::close(int code, const std::string& reason) {
  if (closed_) return;
  closed_ = true;
  if (close_) {
    close_fn fn = std::move(close_);
    close_ = nullptr;
    fn(code, reason);
  }
}

void stream_transport::set_handlers(text_handler on_text, binary_handler on_binary,
                                  close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  on_close_ = std::move(on_close);
}

void stream_transport::feed(const uint8_t* data, std::size_t len) {
  if (closed_ || data == nullptr || len == 0) return;
  buffer_.insert(buffer_.end(), data, data + len);
  std::size_t consumed = 0;
  while (buffer_.size() - consumed >= protocol::kPacketHeaderSize) {
    const uint8_t* packet = buffer_.data() + consumed;
    if (std::memcmp(packet, protocol::kMagic, sizeof(protocol::kMagic)) != 0) {
      deliver_close(transport_close::policy, "bad packet magic on the QUIC stream");
      return;
    }
    const uint32_t payload_len =
        static_cast<uint32_t>(packet[24]) |
        (static_cast<uint32_t>(packet[25]) << 8) |
        (static_cast<uint32_t>(packet[26]) << 16) |
        (static_cast<uint32_t>(packet[27]) << 24);
    if (payload_len > protocol::kMaxPayloadBytes) {
      deliver_close(transport_close::policy, "QUIC frame is too large");
      return;
    }
    const std::size_t total = protocol::kPacketHeaderSize + payload_len;
    if (buffer_.size() - consumed < total) break;
    const uint8_t type = packet[5];
    if (type == static_cast<uint8_t>(protocol::packet_type::control)) {
      if (text_) {
        text_(std::string(reinterpret_cast<const char*>(
                              packet + protocol::kPacketHeaderSize),
                          payload_len));
      }
    } else if (binary_) {
      binary_(packet, total);
    }
    consumed += total;
  }
  if (consumed > 0) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + consumed);
  }
}

void stream_transport::peer_closed(const std::string& reason) {
  deliver_close(transport_close::normal, reason);
}

void stream_transport::deliver_close(int code, const std::string& reason) {
  if (close_delivered_) return;
  close_delivered_ = true;
  closed_ = true;
  if (on_close_) {
    close_handler handler = std::move(on_close_);
    on_close_ = nullptr;
    handler(code, reason);
  }
}

}  // namespace molink
