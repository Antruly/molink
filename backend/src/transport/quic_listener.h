#pragma once

#include <functional>
#include <memory>
#include <string>

#include "transport/stream_transport.h"

namespace uvcpp {
class uvcpp_loop;
}

namespace molink {

/// Accepts MOLK sessions over QUIC on one UDP port.
///
/// The host's TCP listener and this QUIC listener are two doors into the same
/// session controller: a QUIC client opens one bidirectional stream, every
/// MOLK packet after that is framed exactly like the WebSocket binary path,
/// and JSON control messages travel in `packet_type::control` packets. The
/// session path is still "direct" — only the wire protocol differs.
///
/// Threading: like every other libuvcpp endpoint, all methods must run on the
/// loop that owns the UDP socket. `main` posts the start/stop onto the app
/// loop and waits.
class quic_listener {
 public:
  using accept_fn = std::function<void(std::shared_ptr<stream_transport>)>;

  quic_listener(uvcpp::uvcpp_loop* loop, std::string cert_path,
                std::string key_path, accept_fn accept);
  ~quic_listener();

  quic_listener(const quic_listener&) = delete;
  quic_listener& operator=(const quic_listener&) = delete;

  /// Binds @p ip:@p port and starts accepting. The device certificate is
  /// loaded once; a client must offer the "molink" ALPN. Returns false and
  /// fills @p error when the build has no QUIC, the certificate is unusable
  /// or the port cannot be bound.
  bool start(const std::string& ip, int port, std::string& error);
  /// Stops accepting and closes live connections. Safe to call more than once.
  void stop();

  bool listening() const;
  /// The bound port (useful when @p port was 0).
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
