#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_config.h>

#include "transport/relay_session.h"

// Set by CMake. Builds whose libuvcpp has no HTTP/2 keep the members below
// compiled out: their headers (and the timer type) may not be installed, and a
// unique_ptr member with an incomplete type cannot be destroyed.
#ifndef MOLINK_HAVE_H2_WS
#define MOLINK_HAVE_H2_WS 0
#endif

namespace uvcpp {
class uvcpp_h2_connection;
class uvcpp_h2_ws_connection;
class uvcpp_loop;
class uvcpp_ssl_context;
class uvcpp_tcp_client;
class uvcpp_timer;
}  // namespace uvcpp

namespace molink {

/// Relay session carried by a WebSocket over HTTP/2 (RFC 8441). The client
/// opens a TCP/TLS connection to the relay's device port, negotiates ALPN
/// "h2" (or h2c prior knowledge), sends an extended CONNECT to /v1/relay and
/// then the same `{"connect_id","token"}` text handshake as the h3 leg.
class h2_relay_transport final
    : public relay_session,
      public std::enable_shared_from_this<h2_relay_transport> {
 public:
  using post_fn = std::function<void(std::function<void()>)>;

  h2_relay_transport(std::string host, int port, std::string connect_id,
                     std::string token, std::string ca_path, bool secure,
                     uvcpp::uvcpp_loop* loop, post_fn post);
  ~h2_relay_transport() override;

  const char* name() const override { return "http2"; }
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
  void try_connect();
  void on_text(const std::string& text);
  void on_binary(const uint8_t* data, std::size_t len);
  void on_link_closed(int code, const std::string& reason);
  void fail(const std::string& reason);
  void stop_connecting();

  std::string host_;
  int port_ = 0;
  std::string connect_id_;
  std::string token_;
  std::string ca_path_;
  bool secure_ = true;
  uvcpp::uvcpp_loop* loop_ = nullptr;
  post_fn post_;

#if MOLINK_HAVE_H2_WS
  std::unique_ptr<uvcpp::uvcpp_ssl_context> tls_;
  std::unique_ptr<uvcpp::uvcpp_tcp_client> tcp_;
  std::unique_ptr<uvcpp::uvcpp_h2_connection> h2_;
  std::unique_ptr<uvcpp::uvcpp_h2_ws_connection> ws_;
  std::unique_ptr<uvcpp::uvcpp_timer> timer_;
#endif
  std::chrono::steady_clock::time_point deadline_{};

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
