#include "transport/wt_transport.h"

#include <cstring>
#include <utility>

#include <http3/uvcpp_h3_wt_session.h>

#include "protocol/frame_packet.h"
#include "transport/stream_transport.h"

namespace molink {

wt_transport::wt_transport(std::string peer) : peer_(std::move(peer)) {}

wt_transport::~wt_transport() = default;

void wt_transport::attach(uvcpp::uvcpp_h3_wt_session* session) {
  session_ = session;
}

void wt_transport::attach_stream(uvcpp::uvcpp_h3_wt_stream* stream) {
  if (stream == nullptr || reliable_ != nullptr) return;
  reliable_ = stream;
  reliable_->on_data([this](const uint8_t* data, std::size_t len) {
    feed_reliable(data, len);
  });
  reliable_->on_end([this] {
    link_closed(transport_close::normal, "webtransport stream ended");
  });
  // The shared framing lives in stream_transport: it turns a byte pipe into
  // MOLK packets and routes control text to the text handler.
  stream_ = std::make_shared<stream_transport>(
      [this](const uint8_t* data, std::size_t len) {
        if (reliable_ == nullptr || !reliable_->is_open()) return false;
        return reliable_->write(data, len) == 0;
      },
      [this](int code, const std::string& reason) { deliver_close(code, reason); },
      "webtransport", peer_, 0);
  stream_->set_handlers(
      [this](const std::string& text) {
        if (text_) text_(text);
      },
      [this](const uint8_t* data, std::size_t len) {
        if (binary_) binary_(data, len);
      },
      [this](int code, const std::string& reason) {
        deliver_close(code, reason);
      });
}

void wt_transport::link_closed(int code, const std::string& reason) {
  if (closed_) return;
  closed_ = true;
  // The session owns its streams: once the session is gone this pointer is
  // dangling, so drop it before anything can ask is_open() again.
  reliable_ = nullptr;
  session_ = nullptr;
  deliver_close(code, reason);
}

std::string wt_transport::peer_ip() const { return peer_; }

bool wt_transport::is_open() const {
  return !closed_ && session_ != nullptr && reliable_ != nullptr &&
         reliable_->is_open();
}

bool wt_transport::send_text(const std::string& text) {
  return stream_ != nullptr && stream_->send_text(text);
}

bool wt_transport::send_text(transport_channel channel,
                             const std::string& text) {
  if (channel != transport_channel::control) return false;
  return send_text(text);
}

bool wt_transport::send_binary(const uint8_t* data, std::size_t len) {
  return send_binary(transport_channel::control, data, len);
}

bool wt_transport::send_binary(transport_channel channel, const uint8_t* data,
                               std::size_t len) {
  if (!is_open() || (data == nullptr && len > 0)) return false;
  // Audio and video both take the datagram path: they are latency-sensitive
  // and must not queue behind file bytes on the reliable stream.
  const bool datagram = channel == transport_channel::video ||
                        channel == transport_channel::audio;
  if (datagram && session_ != nullptr && len <= kDatagramFrameLimit) {
    const datagram_channel tag = channel == transport_channel::audio
                                     ? datagram_channel::audio
                                     : datagram_channel::video;
    const std::vector<datagram_fragment> fragments =
        fragment_frame(tag, data, len, next_frame_++);
    if (!fragments.empty()) {
      bool ok = true;
      for (const datagram_fragment& fragment : fragments) {
        if (session_->send_datagram(fragment.bytes.data(),
                                    fragment.bytes.size()) != 0) {
          ok = false;
          break;
        }
      }
      // A datagram the stack refused (no datagram support, or a queue that is
      // full) is not worth turning into a reliable send: the frame is already
      // late, and the repair loop covers the loss.
      return ok;
    }
  }
  // Everything else -- control, audio, file, and any frame too large to
  // fragment -- rides the reliable stream.
  return stream_ != nullptr && stream_->send_binary(data, len);
}

void wt_transport::close(int code, const std::string& reason) {
  if (closed_) return;
  closed_ = true;
  if (session_ != nullptr) session_->close(0, reason);
  reliable_ = nullptr;
  session_ = nullptr;
  deliver_close(code, reason);
}

void wt_transport::set_handlers(text_handler on_text, binary_handler on_binary,
                                close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  close_ = std::move(on_close);
}

void wt_transport::feed_reliable(const uint8_t* data, std::size_t len) {
  if (stream_ != nullptr) stream_->feed(data, len);
}

void wt_transport::feed_datagram(const uint8_t* data, std::size_t len) {
  const datagram_channel tag = datagram_channel_of(data, len);
  datagram_reassembler& assembler = tag == datagram_channel::audio
                                        ? audio_reassembly_
                                        : video_reassembly_;
  std::vector<uint8_t> frame;
  if (!assembler.push(data, len, frame)) return;
  if (frame.empty()) return;
  ++frames_received_;
  if (binary_) binary_(frame.data(), frame.size());
}

void wt_transport::deliver_close(int code, const std::string& reason) {
  if (close_delivered_) return;
  close_delivered_ = true;
  closed_ = true;
  if (close_) {
    close_handler handler = std::move(close_);
    close_ = nullptr;
    handler(code, reason);
  }
}

}  // namespace molink
