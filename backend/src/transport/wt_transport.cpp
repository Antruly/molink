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

void wt_transport::attach_peer_stream(uvcpp::uvcpp_h3_wt_stream* stream) {
  if (stream == nullptr) return;
  // The channel tag rides the first data callback, so this handler decides
  // the stream's role once and then keeps routing.
  auto first = std::make_shared<bool>(true);
  stream->on_data([this, stream, first](const uint8_t* data, std::size_t len) {
    if (*first && len > 0) {
      *first = false;
      if (data[0] == kFileStreamTag) {
        attach_file_stream(stream);
        if (len > 1) feed_file(data + 1, len - 1);
        return;
      }
      attach_control_stream(stream);
      feed_reliable(data, len);
      return;
    }
    if (stream == file_stream_) {
      feed_file(data, len);
    } else {
      feed_reliable(data, len);
    }
  });
}

void wt_transport::attach_control_stream(uvcpp::uvcpp_h3_wt_stream* stream) {
  if (reliable_ != nullptr || stream == nullptr) return;
  reliable_ = stream;
  reliable_->on_end([this] {
    link_closed(transport_close::normal, "webtransport stream ended");
  });
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

void wt_transport::attach_file_stream(uvcpp::uvcpp_h3_wt_stream* stream) {
  if (file_stream_ != nullptr || stream == nullptr) return;
  file_stream_ = stream;
  // Its own framing instance: a file chunk must not interleave with control
  // bytes on the shared parser, and it has its own write path.
  file_transport_ = std::make_shared<stream_transport>(
      [this](const uint8_t* data, std::size_t len) {
        if (file_stream_ == nullptr || !file_stream_->is_open()) return false;
        return file_stream_->write(data, len) == 0;
      },
      [this](int, const std::string&) {},
      "webtransport", peer_, 0);
  file_transport_->set_handlers(
      [this](const std::string& text) {
        if (text_) text_(text);
      },
      [this](const uint8_t* data, std::size_t len) {
        if (binary_) binary_(data, len);
      },
      [this](int, const std::string&) {});
}

void wt_transport::feed_file(const uint8_t* data, std::size_t len) {
  if (file_transport_ != nullptr) file_transport_->feed(data, len);
}

void wt_transport::link_closed(int code, const std::string& reason) {
  if (closed_) return;
  closed_ = true;
  // The session owns its streams: once the session is gone this pointer is
  // dangling, so drop it before anything can ask is_open() again.
  reliable_ = nullptr;
  file_stream_ = nullptr;
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
  // Files get their own stream when the peer opened one: a transfer cannot
  // then sit in front of control messages.
  if (channel == transport_channel::file && file_transport_ != nullptr) {
    return file_transport_->send_binary(data, len);
  }
  // Everything else -- control, and any frame too large to fragment -- rides
  // the control stream.
  return stream_ != nullptr && stream_->send_binary(data, len);
}

void wt_transport::close(int code, const std::string& reason) {
  if (closed_) return;
  closed_ = true;
  if (session_ != nullptr) session_->close(0, reason);
  reliable_ = nullptr;
  file_stream_ = nullptr;
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
