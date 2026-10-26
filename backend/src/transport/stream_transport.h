#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "transport/session_transport.h"

namespace molink {

/// A session transport over one reliable byte stream (a QUIC stream or a TCP
/// connection). MOLINK packets are self-delimiting, so the stream carries one
/// packet after another. JSON control messages (hello/welcome/input) are
/// wrapped in a `packet_type::control` packet; everything else is forwarded as
/// the whole MOLK packet, exactly like the WebSocket and ICE transports.
///
/// The `protocol` argument is only the label the welcome and the toolbar
/// report ("quic" or "tcp"); the framing is identical.
class stream_transport final : public session_transport {
 public:
  /// Writes bytes to the stream; false when the stream is gone.
  using write_fn = std::function<bool(const uint8_t* data, std::size_t len)>;
  using close_fn = std::function<void(int code, const std::string& reason)>;

  stream_transport(write_fn write, close_fn close,
                   const char* protocol = "quic",
                   std::string peer_ip = std::string(),
                   unsigned int peer_port = 0);
  ~stream_transport() override;

  const char* name() const override { return protocol_; }
  std::string peer_ip() const override { return peer_ip_; }
  unsigned int peer_port() const override { return peer_port_; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;

  /// Bytes received on the stream, in arrival order.
  void feed(const uint8_t* data, std::size_t len);
  /// The peer finished its side of the stream.
  void peer_closed(const std::string& reason);

 private:
  void deliver_close(int code, const std::string& reason);

  const char* protocol_;
  std::string peer_ip_;
  unsigned int peer_port_ = 0;
  write_fn write_;
  close_fn close_;
  text_handler text_;
  binary_handler binary_;
  close_handler on_close_;
  std::vector<uint8_t> buffer_;
  bool closed_ = false;
  bool close_delivered_ = false;
};

}  // namespace molink
