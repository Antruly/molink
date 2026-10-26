#pragma once

#include <string>

namespace molink {

/// The subset of a peer core's `GET /api/transports` answer this client uses.
struct peer_transports {
  /// The host answered the HTTP request.
  bool reachable = false;
  /// UDP port of an available QUIC session listener; 0 when the peer does not
  /// offer one (older build, listener failed to bind, or a protocol ladder
  /// that ranks something else first).
  int quic_port = 0;
  /// Plain-TCP data port, when the peer advertises the TCP rung.
  int tcp_port = 0;
  /// The peer's web app accepts HTTP/2 WebSocket (RFC 8441) on its web port.
  bool http2 = false;
  /// The peer's transport certificate (PEM) and its SHA-256 fingerprint, when
  /// the peer publishes them for pinning.
  std::string tls_cert;
  std::string tls_fingerprint;
  /// Why the probe failed, when reachable is false.
  std::string error;
};

/// Asks `http://host:port/api/transports` what the peer can carry.
///
/// This is a blocking socket call (libuvcpp's wait-style HTTP client), so it
/// must run on a worker thread, never on the app loop. It never throws.
peer_transports probe_peer_transports(const std::string& host, int port,
                                      int timeout_ms = 2500);

}  // namespace molink
