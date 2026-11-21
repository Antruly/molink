#include "transport/switchable_transport.h"

#include <cstring>
#include <utility>

#include "protocol/frame_packet.h"

namespace molink {
namespace {

/// File bytes are the one payload that cannot tolerate UDP loss: the custom
/// UDP fragmentation has no retransmission, so a single missing fragment
/// stalls the whole transfer. Keep the relay (WebSocket over TCP) alive after
/// the P2P upgrade and route file_data packets there. Frames, voice and input
/// stay on the low-latency ICE path.
bool is_file_data(const uint8_t* data, std::size_t len) {
  return data != nullptr && len >= protocol::kPacketHeaderSize &&
         std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) == 0 &&
         data[5] == static_cast<uint8_t>(protocol::packet_type::file_data);
}

/// Legacy standalone AV1 frames stay on the relay like file bytes: UDP
/// fragmentation cannot repair a missing fragment. AV1 *patches* are sent as
/// tile refreshes, so is_video_repair moves those with the video once ICE is
/// up.
bool is_av1_frame(const uint8_t* data, std::size_t len) {
  return data != nullptr &&
         len >= protocol::kPacketHeaderSize + protocol::kFrameHeaderSize &&
         std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) == 0 &&
         data[5] == static_cast<uint8_t>(protocol::packet_type::frame) &&
         data[protocol::kPacketHeaderSize + 6] ==
             static_cast<uint8_t>(protocol::codec::av1);
}

/// A keyframe is the re-anchor the decoder cannot produce for itself, and a
/// tile refresh replaces pixels the client missed. Both travel with the video
/// once ICE is active so a half-open relay cannot swallow them; before the
/// switch (or without ICE) they use the relay.
bool is_tile_refresh(const uint8_t* data, std::size_t len) {
  return data != nullptr &&
         len >= protocol::kPacketHeaderSize + protocol::kFrameHeaderSize &&
         std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) == 0 &&
         data[5] == static_cast<uint8_t>(protocol::packet_type::frame) &&
         (data[6] & static_cast<uint8_t>(protocol::kPacketFlagTileRefresh)) != 0;
}

/// The tile clock is the host's side of the same repair loop. It is one small
/// packet per second and follows the video path, together with the repairs it
/// schedules.
bool is_tile_clock(const uint8_t* data, std::size_t len) {
  return data != nullptr && len >= protocol::kPacketHeaderSize &&
         std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) == 0 &&
         data[5] == static_cast<uint8_t>(protocol::packet_type::tile_clock);
}

bool is_keyframe(const uint8_t* data, std::size_t len) {
  return data != nullptr &&
         len >= protocol::kPacketHeaderSize + protocol::kFrameHeaderSize &&
         std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) == 0 &&
         data[5] == static_cast<uint8_t>(protocol::packet_type::frame) &&
         (data[protocol::kPacketHeaderSize + 7] &
          protocol::kFrameFlagKeyframe) != 0;
}

/// Binary packets that may legitimately arrive on the relay. The receiving
/// side accepts them so an older peer (which sends repairs over the relay)
/// keeps working; this build sends the video duplicates over ICE instead.
/// It used to accept only file_data, which silently dropped every keyframe,
/// AV1 patch, tile refresh and tile clock after the P2P switch.
bool rides_relay(const uint8_t* data, std::size_t len) {
  return is_file_data(data, len) || is_av1_frame(data, len) ||
         is_keyframe(data, len) || is_tile_refresh(data, len) ||
         is_tile_clock(data, len);
}

/// Packets that keep the picture decodable: the re-anchor keyframe, the
/// pixel-level repair and the clock that asks for repairs. These follow the
/// video once ICE is up. The relay is not a safe place for them after the
/// P2P switch: it can go half-open, after which every keyframe and repair is
/// silently swallowed while deltas over ICE keep arriving. The result is a
/// frozen picture that no repair request can recover.
bool is_video_repair(const uint8_t* data, std::size_t len) {
  return is_keyframe(data, len) || is_tile_refresh(data, len) ||
         is_tile_clock(data, len);
}

}  // namespace

switchable_transport::switchable_transport(
    std::shared_ptr<session_transport> initial)
    : active_(std::move(initial)), relay_(active_) {}

const char* switchable_transport::name() const {
  return active_ ? active_->name() : "websocket";
}

const char* switchable_transport::path() const {
  return active_ ? active_->path() : "relay";
}

std::string switchable_transport::peer_ip() const {
  return active_ ? active_->peer_ip() : std::string("relay");
}

unsigned int switchable_transport::peer_port() const {
  return active_ ? active_->peer_port() : 0;
}

bool switchable_transport::is_open() const {
  return !closed_ && active_ && active_->is_open();
}

transport_capabilities switchable_transport::capabilities() const {
  transport_capabilities result;
  if (!active_) return result;
  result = active_->capabilities();
  if (!result.independent_file && relay_ && relay_->is_open()) {
    result.independent_file = true;
  }
  return result;
}

bool switchable_transport::ice_active() const {
  return ice_ != nullptr && active_.get() == ice_.get();
}

bool switchable_transport::send_text(const std::string& text) {
  return !closed_ && active_ && active_->send_text(text);
}

bool switchable_transport::send_text(transport_channel channel,
                                     const std::string& text) {
  return !closed_ && active_ && active_->send_text(channel, text);
}

bool switchable_transport::send_binary(const uint8_t* data, std::size_t len) {
  if (closed_) return false;
  if (is_file_data(data, len)) {
    return relay_ && relay_->is_open() && relay_->send_binary(data, len);
  }
  if (is_video_repair(data, len) && ice_active()) {
    return active_ && active_->send_binary(data, len);
  }
  if (rides_relay(data, len) && relay_ && relay_->is_open()) {
    return relay_->send_binary(data, len);
  }
  return active_ && active_->send_binary(data, len);
}

bool switchable_transport::send_binary(transport_channel channel,
                                       const uint8_t* data,
                                       std::size_t len) {
  if (closed_) return false;
  if (channel == transport_channel::file || is_file_data(data, len)) {
    return relay_ && relay_->is_open() &&
           relay_->send_binary(transport_channel::file, data, len);
  }
  if (channel == transport_channel::video && is_video_repair(data, len) &&
      ice_active()) {
    return active_ && active_->send_binary(channel, data, len);
  }
  if (channel == transport_channel::video && rides_relay(data, len) && relay_ &&
      relay_->is_open()) {
    return relay_->send_binary(channel, data, len);
  }
  return active_ && active_->send_binary(channel, data, len);
}

void switchable_transport::close(int code, const std::string& reason) {
  if (closed_) return;
  closed_ = true;
  if (relay_) relay_->close(code, reason);
  if (ice_) ice_->close(code, reason);
  active_.reset();
}

void switchable_transport::set_handlers(text_handler on_text,
                                        binary_handler on_binary,
                                        close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  close_ = std::move(on_close);
  if (relay_) wire(relay_);
  if (ice_) wire(ice_);
}

void switchable_transport::wire(
    const std::shared_ptr<session_transport>& transport) {
  if (!transport) return;
  auto source = transport.get();
  auto weak = weak_from_this();
  transport->set_handlers(
      [weak, source](const std::string& text) {
        auto self = weak.lock();
        if (!self || self->closed_ || !self->text_) return;
        // A peer can still be sending on relay while its ICE checks finish.
        // Each message is sent once; accepting both owned paths preserves
        // queued input, hello and file control without duplicating execution.
        // JSON parsing remains the session controller's responsibility.
        if (self->active_.get() != source && self->relay_.get() != source) return;
        self->text_(text);
      },
      [weak, source](const uint8_t* data, std::size_t len) {
        auto self = weak.lock();
        if (!self || self->closed_ || !self->binary_) return;
        if (self->active_.get() != source && !rides_relay(data, len)) return;
        self->binary_(data, len);
      },
      [weak, source](int code, const std::string& reason) {
        auto self = weak.lock();
        if (!self || self->closed_ || self->active_.get() != source) return;
        self->closed_ = true;
        if (self->relay_ && self->relay_.get() != source) {
          self->relay_->close(code, reason);
        }
        if (self->close_) self->close_(code, reason);
      });
}

void switchable_transport::set_switch_handler(std::function<void()> handler) {
  switch_handler_ = std::move(handler);
}

void switchable_transport::set_ice(
    std::shared_ptr<session_transport> ice) {
  if (!ice || switched_ || closed_) return;
  ice_ = std::move(ice);
  wire(ice_);
  switched_ = true;
  active_ = ice_;
  // Keep the relay connected for reliable file data. Losing it rejects new
  // file blocks without closing the ICE session or sending them unreliably.
  if (switch_handler_) switch_handler_();
}

}  // namespace molink
