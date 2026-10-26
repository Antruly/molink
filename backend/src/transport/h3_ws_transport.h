#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <uvcpp/uvcpp_config.h>

#include "transport/session_transport.h"

#ifndef MOLINK_HAVE_H3_WS
// Set by CMake when the installed libuvcpp ships the HTTP/3 WebSocket header.
#define MOLINK_HAVE_H3_WS 0
#endif

#if MOLINK_HAVE_H3_WS
namespace uvcpp {
class uvcpp_h3_ws_connection;
}
#endif

namespace molink {

/// A MOLK session over a WebSocket carried by HTTP/3 (RFC 9220 extended
/// CONNECT, `:protocol = websocket`).
///
/// The message shape is the WebSocket one, not the raw QUIC stream: text
/// control messages and whole MOLK packets travel as WS messages, so the
/// session controller can use it exactly like ws_transport. The listener owns
/// the h3 connection and feeds this adapter.
class h3_ws_transport final : public session_transport {
 public:
  h3_ws_transport(std::string peer_ip);
  ~h3_ws_transport() override;

  const char* name() const override { return "http3"; }
  const char* path() const override { return "direct"; }
  std::string peer_ip() const override { return peer_ip_; }
  unsigned int peer_port() const override { return 0; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;

#if MOLINK_HAVE_H3_WS
  /// The WebSocket session for this transport (owned by the listener).
  void attach(uvcpp::uvcpp_h3_ws_connection* ws);
  /// Feed an inbound text/binary message from the listener.
  void deliver_text(const std::string& text);
  void deliver_binary(const uint8_t* data, std::size_t len);
  /// The h3 stream/connection ended. Clears the WebSocket and reports once.
  void link_closed(int code, const std::string& reason);
#endif

 private:
  std::string peer_ip_;
#if MOLINK_HAVE_H3_WS
  uvcpp::uvcpp_h3_ws_connection* ws_ = nullptr;
#endif
  bool closed_ = false;
  bool close_delivered_ = false;
  text_handler text_;
  binary_handler binary_;
  close_handler on_close_;
};

}  // namespace molink
