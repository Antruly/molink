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

/// AV1 patches are large (one message per changed rectangle) and UDP
/// fragmentation cannot repair a missing fragment, so keep them on the relay
/// just like file bytes. Tile frames are small enough to stay on ICE.
bool is_av1_frame(const uint8_t* data, std::size_t len) {
  return data != nullptr &&
         len >= protocol::kPacketHeaderSize + protocol::kFrameHeaderSize &&
         std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) == 0 &&
         data[5] == static_cast<uint8_t>(protocol::packet_type::frame) &&
         data[protocol::kPacketHeaderSize + 6] ==
             static_cast<uint8_t>(protocol::codec::av1);
}

/// A keyframe is the only frame a client cannot recover without: if its UDP
/// fragments are lost the picture never appears. Keep it on the reliable
/// relay path; deltas and tile refreshes stay on ICE.
/// Tile refreshes are the repair for pixels the client missed; losing them
/// over UDP defeats the whole compensation path, so they take the relay too.
bool is_tile_refresh(const uint8_t* data, std::size_t len) {
  return data != nullptr &&
         len >= protocol::kPacketHeaderSize + protocol::kFrameHeaderSize &&
         std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) == 0 &&
         data[5] == static_cast<uint8_t>(protocol::packet_type::frame) &&
         (data[6] & static_cast<uint8_t>(protocol::kPacketFlagTileRefresh)) != 0;
}

/// Repair requests must reach the host; one lost UDP datagram would otherwise
/// leave the stale rectangle untouched until something else drops.
bool is_compensation_text(const std::string& text) {
  return text.find("\"type\":\"request-tiles\"") != std::string::npos ||
         text.find("\"type\":\"request-keyframe\"") != std::string::npos;
}

/// The tile clock is the host's side of the same repair loop. It is one small
/// packet per second, so the reliable path costs nothing; keeping it off ICE
/// means a lossy link cannot silence the clock that is supposed to notice the
/// very losses it creates.
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

/// The binary packets that must ride the relay even after ICE takes over.
/// The receiving side has to accept exactly this set from the relay; it used
/// to accept only file_data, which silently dropped every keyframe, AV1
/// patch, tile refresh and tile clock after the P2P switch.
bool rides_relay(const uint8_t* data, std::size_t len) {
  return is_file_data(data, len) || is_av1_frame(data, len) ||
         is_keyframe(data, len) || is_tile_refresh(data, len) ||
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

bool switchable_transport::send_text(const std::string& text) {
  if (is_compensation_text(text) && relay_ && relay_->is_open()) {
    return relay_->send_text(text);
  }
  return active_ && active_->send_text(text);
}

bool switchable_transport::send_binary(const uint8_t* data, std::size_t len) {
  if (rides_relay(data, len) && relay_ && relay_->is_open()) {
    return relay_->send_binary(data, len);
  }
  return active_ && active_->send_binary(data, len);
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
        // After the P2P switch the relay carries the packets that must not be
        // lost: file data, keyframes, AV1 patches, tile refreshes and the
        // tile clock. Repair requests travel the other way on the same path.
        // Everything else still arrives on the active transport.
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
