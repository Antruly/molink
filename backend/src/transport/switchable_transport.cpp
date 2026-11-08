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

/// Repair requests must reach the host. They follow the same rule as the
/// repairs they ask for: relay before the switch, ICE afterwards.
bool is_compensation_text(const std::string& text) {
  return text.find("\"type\":\"request-tiles\"") != std::string::npos ||
         text.find("\"type\":\"request-keyframe\"") != std::string::npos;
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
    : active_(std::move(initial)), relay_(active_) {
  wire(relay_);
}

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
  // Repair requests are part of the video path. Once ICE is up they must not
  // be left to a relay that may already be half-open: the host would never
  // learn that the viewer needs a keyframe.
  if (is_compensation_text(text) && !ice_active() && relay_ &&
      relay_->is_open()) {
    return relay_->send_text(text);
  }
  return active_ && active_->send_text(text);
}

bool switchable_transport::send_text(transport_channel channel,
                                     const std::string& text) {
  if (channel == transport_channel::control && is_compensation_text(text) &&
      !ice_active() && relay_ && relay_->is_open()) {
    return relay_->send_text(channel, text);
  }
  return active_ && active_->send_text(channel, text);
}

bool switchable_transport::send_binary(const uint8_t* data, std::size_t len) {
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
  if (channel == transport_channel::file && relay_ && relay_->is_open()) {
    return relay_->send_binary(channel, data, len);
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
  transport->set_handlers(
      [this, source](const std::string& text) {
        if (closed_ || !text_) return;
        // After the P2P switch the relay carries file data and AV1 patches.
        // This build sends video repairs with the video on ICE, but still
        // accept relay-borne keyframes, refreshes and repair requests: the
        // peer may be an older build, and dropping them would freeze its
        // picture. Everything else arrives on the active transport.
        if (active_.get() != source && !is_compensation_text(text)) return;
        text_(text);
      },
      [this, source](const uint8_t* data, std::size_t len) {
        if (closed_ || !binary_) return;
        if (active_.get() != source && !rides_relay(data, len)) return;
        binary_(data, len);
      },
      [this, source](int code, const std::string& reason) {
        if (closed_ || active_.get() != source || !close_) return;
        closed_ = true;
        close_(code, reason);
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
  // Keep the relay connected for file_data (see is_file_data): control,
  // frames and voice use ICE, files stay on reliable TCP. The relay close
  // handler ignores events while it is not active, so losing the relay only
  // drops file transfers back to UDP instead of killing the session.
  if (switch_handler_) switch_handler_();
}

}  // namespace molink
