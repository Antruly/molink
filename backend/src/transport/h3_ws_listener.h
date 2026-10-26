#pragma once

#include <functional>
#include <memory>
#include <string>

#include "transport/session_transport.h"

namespace uvcpp {
class uvcpp_loop;
}

namespace molink {

/// Accepts MOLK sessions over WebSocket-on-HTTP/3 (RFC 9220).
///
/// A browser (or another core) opens an extended CONNECT with
/// `:protocol = websocket` to this listener's port; the accepted WebSocket is
/// adapted to `session_transport`, so the session controller is unchanged.
/// The transport reports protocol "http3" and path "direct"; the UI therefore
/// labels it HTTP/3 rather than WS.
///
/// Requires libuvcpp built with HTTP/3 (UVCPP_HTTP3_ENABLE); start() fails
/// cleanly otherwise. Threading: app loop only.
class h3_ws_listener {
 public:
  using accept_fn = std::function<void(std::shared_ptr<session_transport>)>;
  using post_fn = std::function<void(std::function<void()>)>;

  h3_ws_listener(uvcpp::uvcpp_loop* loop, std::string cert_path,
                 std::string key_path, accept_fn accept, post_fn post);
  ~h3_ws_listener();

  h3_ws_listener(const h3_ws_listener&) = delete;
  h3_ws_listener& operator=(const h3_ws_listener&) = delete;

  bool start(const std::string& ip, int port, std::string& error);
  void stop();

  bool listening() const;
  int port() const;

 private:
  struct impl;
  std::shared_ptr<impl> impl_;
};

}  // namespace molink
