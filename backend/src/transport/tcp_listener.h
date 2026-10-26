#pragma once

#include <functional>
#include <memory>
#include <string>

#include "transport/stream_transport.h"

namespace uvcpp {
class uvcpp_loop;
}

namespace molink {

/// Accepts MOLK sessions over a plain TCP connection (`--tcp-data-port`).
///
/// This is the direct client-to-client TCP rung of the transport ladder:
/// the same self-delimiting MOLK stream as QUIC, with the session path still
/// "direct". Plain TCP matches the existing plain `ws://` LAN baseline; the
/// verification code still authenticates the session.
///
/// Threading: like every libuvcpp endpoint, all methods must run on the loop
/// that owns the listening socket.
class tcp_listener {
 public:
  using accept_fn = std::function<void(std::shared_ptr<stream_transport>)>;

  /// The listener is TLS-only: sessions are pinned to the device certificate.
  tcp_listener(uvcpp::uvcpp_loop* loop, std::string cert_path,
               std::string key_path, accept_fn accept);
  ~tcp_listener();

  tcp_listener(const tcp_listener&) = delete;
  tcp_listener& operator=(const tcp_listener&) = delete;

  /// Binds @p ip:@p port and starts accepting. Returns false and fills
  /// @p error when the address cannot be bound.
  bool start(const std::string& ip, int port, std::string& error);
  /// Stops accepting and closes live connections. Idempotent.
  void stop();

  bool listening() const;
  int port() const;

 private:
  struct impl;

  uvcpp::uvcpp_loop* loop_;
  std::string cert_path_;
  std::string key_path_;
  accept_fn accept_;
  std::unique_ptr<impl> impl_;
};

}  // namespace molink
