#pragma once

#include <memory>
#include <string>

#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_web_ws.h>

#include "session_controller.h"
#include "transport/session_transport.h"

namespace molink {

/// Handler for WS /ws/session. The adapter wraps the connection in
/// ws_transport and hands it to session_controller, which owns all session
/// logic.
uvcpp::uvcpp_web_ws_handler make_ws_handler(
    std::shared_ptr<session_registry> registry, session_options options);

/// WebSocket implementation of session_transport. The connection is owned by
/// libuvcpp; this wrapper only borrows it while the session is alive.
class ws_transport final : public session_transport {
 public:
  ws_transport(uvcpp::uvcpp_ws_connection* conn, std::string peer_ip,
               unsigned int peer_port);

  const char* name() const override { return "websocket"; }
  std::string peer_ip() const override { return peer_ip_; }
  unsigned int peer_port() const override { return peer_port_; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;

 private:
  uvcpp::uvcpp_ws_connection* conn_ = nullptr;
  std::string peer_ip_;
  unsigned int peer_port_ = 0;

  text_handler text_;
  binary_handler binary_;
  close_handler close_;
};

}  // namespace molink
