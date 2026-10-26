#pragma once

#include <cstddef>
#include <string>

#include "transport/session_transport.h"

namespace uvcpp {
class uvcpp_h2_ws_connection;
}

namespace molink {

/// Client-side adapter for a WebSocket over HTTP/2 (RFC 8441) opened on an
/// `uvcpp_h2_connection`. The direct proxy owns the h2 connection and the WS
/// object; this wrapper borrows the WS and exposes it as a session transport.
class h2_ws_transport final : public session_transport {
 public:
  explicit h2_ws_transport(std::string peer_ip,
                           unsigned int peer_port = 0);
  ~h2_ws_transport() override;

  const char* name() const override { return "http2"; }
  const char* path() const override { return "direct"; }
  std::string peer_ip() const override { return peer_ip_; }
  unsigned int peer_port() const override { return peer_port_; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;

  void attach(uvcpp::uvcpp_h2_ws_connection* ws);
  void deliver_text(const std::string& text);
  void deliver_binary(const uint8_t* data, std::size_t len);
  void link_closed(int code, const std::string& reason);

 private:
  std::string peer_ip_;
  unsigned int peer_port_ = 0;
  uvcpp::uvcpp_h2_ws_connection* ws_ = nullptr;
  bool closed_ = false;
  bool close_delivered_ = false;

  text_handler text_;
  binary_handler binary_;
  close_handler on_close_;
};

}  // namespace molink
