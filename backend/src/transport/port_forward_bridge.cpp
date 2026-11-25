#include "transport/port_forward_bridge.h"

#include <cstring>
#include <utility>
#include <vector>

#include "protocol/frame_packet.h"

namespace molink {
namespace {

bool is_forward_type(uint8_t type) {
  return type == static_cast<uint8_t>(protocol::packet_type::forward_data);
}

}  // namespace

struct port_forward_bridge::link final : public port_forward_link {
  link(std::shared_ptr<session_transport> transport, post_fn post)
      : transport_(std::move(transport)), post_(std::move(post)) {}

  /// Relay workers call this from their own threads. Only the latch is read:
  /// touching the loop-affine transport here would race with its close.
  bool is_open() const override { return open_.load(); }

  void mark_closed() override { open_.store(false); }

  bool send_text(const std::string& text) override {
    if (!open_.load()) return false;
    auto transport = transport_;
    if (!transport) return false;
    auto deliver = [transport, text] {
      if (transport->is_open()) {
        // Control, not file: the plain WebSocket relay transport used by the
        // /ws/direct fallback only accepts text on the control channel.
        // Ordering against FORWARD_DATA is guaranteed by the open handshake:
        // the manager sends no bytes until forward-open-ok arrives.
        transport->send_text(transport_channel::control, text);
      }
    };
    if (post_) {
      post_(std::move(deliver));
      return true;
    }
    deliver();
    return true;
  }

  bool send_data(uint16_t stream, const uint8_t* data,
                 std::size_t len) override {
    if (!open_.load()) return false;
    auto transport = transport_;
    if (!transport) return false;
    const std::vector<uint8_t> payload =
        encode_port_forward_payload(stream, data, len);
    std::vector<uint8_t> packet;
    std::string error;
    if (!protocol::encode_packet_into(
            packet, static_cast<uint8_t>(protocol::packet_type::forward_data),
            0, 0, 0, 0, payload.data(), payload.size(), error)) {
      return false;
    }
    auto deliver = [transport, packet = std::move(packet)] {
      if (transport->is_open()) {
        transport->send_binary(transport_channel::file, packet.data(),
                               packet.size());
      }
    };
    if (post_) {
      post_(std::move(deliver));
      return true;
    }
    deliver();
    return true;
  }

 private:
  std::shared_ptr<session_transport> transport_;
  post_fn post_;
  std::atomic<bool> open_{true};
};

port_forward_bridge::port_forward_bridge(
    std::shared_ptr<port_forward_manager> manager,
    std::shared_ptr<session_transport> transport, post_fn post)
    : manager_(std::move(manager)),
      transport_(std::move(transport)),
      post_(std::move(post)) {}

port_forward_bridge::~port_forward_bridge() { detach(); }

void port_forward_bridge::attach() {
  if (!manager_ || !transport_) return;
  if (attached_.exchange(true)) return;
  link_ = std::make_shared<link>(transport_, post_);
  manager_->attach_link(link_);
}

void port_forward_bridge::detach() {
  if (!attached_.exchange(false)) return;
  std::shared_ptr<link> link = std::move(link_);
  if (link && manager_) manager_->detach_link_if(link.get());
  if (link) link->mark_closed();
}

bool port_forward_bridge::handle_text(const std::string& text) {
  if (!manager_ || !link_) return false;
  // Cheap gate: ordinary control traffic (cursor, clipboard, stats) never
  // contains this token, so the JSON decoder only runs for forward messages.
  if (text.find("forward-open") == std::string::npos &&
      text.find("forward-close") == std::string::npos) {
    return false;
  }
  port_forward_message message;
  std::string error;
  if (decode_port_forward_message(text, message, error)) {
    manager_->on_control(text);
  }
  // Even a malformed forward message is ours: never show it to the page.
  return true;
}

bool port_forward_bridge::handle_binary(const uint8_t* data, std::size_t len) {
  if (!manager_ || !link_ || data == nullptr) return false;
  if (len < protocol::kPacketHeaderSize) return false;
  if (std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) != 0) {
    return false;
  }
  if (!is_forward_type(data[5])) return false;
  protocol::packet packet;
  std::string error;
  if (!protocol::decode_packet(data, len, packet, error)) {
    // A broken packet on the forward channel is not page data either.
    return true;
  }
  uint16_t stream = 0;
  const uint8_t* payload = nullptr;
  std::size_t payload_len = 0;
  if (decode_port_forward_payload(packet.payload.data(), packet.payload.size(),
                                  stream, payload, payload_len)) {
    manager_->on_data(stream, payload, payload_len);
  }
  return true;
}

}  // namespace molink
