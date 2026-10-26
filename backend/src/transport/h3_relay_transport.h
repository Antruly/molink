#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_config.h>

#include "transport/relay_session.h"

// Set by CMake. Without HTTP/3 the H3 headers are not installed and a
// unique_ptr member with an incomplete type cannot be destroyed.
#ifndef MOLINK_HAVE_H3_WS
#define MOLINK_HAVE_H3_WS 0
#endif

namespace uvcpp {
class uvcpp_loop;
class uvcpp_quic_client;
class uvcpp_h3_connection;
class uvcpp_h3_ws_connection;
class uvcpp_ssl_context;
}

namespace molink {

/// A relay session carried by WebSocket over HTTP/3 (RFC 9220).
///
/// The client opens one QUIC connection with ALPN "h3", sends an extended
/// CONNECT with `:protocol = websocket`, then one text message
/// `{"connect_id":"...","token":"..."}`. The server answers
/// `{"type":"relay-ready"}` as a text message; after that, session text and
/// binary messages are forwarded message-for-message, like the WebSocket
/// relay but over a reliable QUIC stream (no fragmentation needed).
///
/// Threading: app loop only.
class h3_relay_transport final
    : public relay_session,
      public std::enable_shared_from_this<h3_relay_transport> {
 public:
  using post_fn = std::function<void(std::function<void()>)>;

  h3_relay_transport(std::string host, int port, std::string connect_id,
                     std::string token, std::string ca_path,
                     uvcpp::uvcpp_loop* loop, post_fn post);
  ~h3_relay_transport() override;

  const char* name() const override { return "http3"; }
  const char* path() const override { return "relay"; }
  std::string peer_ip() const override { return "relay"; }
  unsigned int peer_port() const override { return 0; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;
  void set_ready_handler(std::function<void()> ready) override;
  void start() override;

 private:
  void on_connected(int status);
  void on_text(const std::string& text);
  void on_binary(const uint8_t* data, std::size_t len);
  void on_link_closed(int code, const std::string& reason);
  void deliver_close(int code, const std::string& reason);
  void fail(const std::string& reason);

  std::string host_;
  int port_ = 0;
  std::string connect_id_;
  std::string token_;
  std::string ca_path_;
  uvcpp::uvcpp_loop* loop_;
  post_fn post_;

  // Destruction order matters: the WebSocket borrows the h3 connection,
  // which borrows the QUIC connection and its TLS context.
  std::shared_ptr<uvcpp::uvcpp_ssl_context> tls_;
  std::shared_ptr<uvcpp::uvcpp_quic_client> quic_;
#if MOLINK_HAVE_H3_WS
  std::unique_ptr<uvcpp::uvcpp_h3_connection> h3_;
  std::unique_ptr<uvcpp::uvcpp_h3_ws_connection> ws_;
#endif

  bool started_ = false;
  bool ready_ = false;
  bool closed_ = false;
  bool close_delivered_ = false;

  text_handler text_;
  binary_handler binary_;
  close_handler on_close_;
  std::function<void()> ready_handler_;
};

}  // namespace molink
