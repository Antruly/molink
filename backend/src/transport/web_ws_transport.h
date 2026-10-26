#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "transport/session_transport.h"

namespace uvcpp {
class uvcpp_web_ws_session;
}

namespace molink {

/// Adapts the webapp's transport-agnostic extended-CONNECT session (HTTP/2
/// RFC 8441 and HTTP/3 RFC 9220) to session_transport. The framework owns the
/// session; the transport borrows it and installs the inbound callbacks, so
/// the handler must keep this object alive until link_closed().
class web_ws_transport final
    : public session_transport,
      public std::enable_shared_from_this<web_ws_transport> {
 public:
  explicit web_ws_transport(std::string protocol);
  ~web_ws_transport() override;

  void set_peer(std::string ip, unsigned int port);

  const char* name() const override { return protocol_.c_str(); }
  const char* path() const override { return "direct"; }
  std::string peer_ip() const override { return peer_ip_; }
  unsigned int peer_port() const override { return peer_port_; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;

  /// Borrow a framework-owned session and install its callbacks.
  void attach(uvcpp::uvcpp_web_ws_session* session);
  /// Called from the session's inbound callbacks.
  void deliver_text(const std::string& text);
  void deliver_binary(const uint8_t* data, std::size_t len);
  void link_closed(int code, const std::string& reason);

 private:
  std::string protocol_;
  std::string peer_ip_;
  unsigned int peer_port_ = 0;
  uvcpp::uvcpp_web_ws_session* session_ = nullptr;
  bool closed_ = false;
  bool close_delivered_ = false;

  text_handler text_;
  binary_handler binary_;
  close_handler on_close_;
};

}  // namespace molink
