#pragma once

#include <atomic>

namespace molink {

// moLink's own port block. It deliberately avoids RustDesk's 21115-21119 and
// TeamViewer's 5938. Each transport gets a stable default so firewall rules
// and bookmarks survive upgrades; all values are configurable and persisted.
//
//   28418  web/API: HTTP/1.1, WebSocket and h2c (one listener)
//   28419  HTTP/3 (QUIC) web/API           [reserved until the QUIC front-end lands]
//   28420  QUIC direct data                [reserved]
//   28421  TCP direct data                 [reserved]
//   28422  UDP direct data                 [reserved]
inline constexpr int kDefaultTcpPort = 28418;
inline constexpr int kDefaultHttp3Port = 28419;
inline constexpr int kDefaultQuicPort = 28420;
inline constexpr int kDefaultTcpDataPort = 28421;
inline constexpr int kDefaultUdpPort = 28422;
/// WebTransport has its own endpoint: its certificate is short-lived (browsers
/// reject serverCertificateHashes otherwise) while the device identity is not.
inline constexpr int kDefaultWebTransportPort = 28423;

struct network_settings {
  /// HTTP/1.1 + WebSocket + h2c listener.
  std::atomic<int> tcp_port{kDefaultTcpPort};
  /// Reserved transport endpoints. They are persisted and advertised now so
  /// clients can negotiate before the data-plane lands.
  std::atomic<int> http3_port{kDefaultHttp3Port};
  std::atomic<int> quic_port{kDefaultQuicPort};
  std::atomic<int> tcp_data_port{kDefaultTcpDataPort};
  std::atomic<int> udp_port{kDefaultUdpPort};
  /// WebTransport endpoint (h3, ':protocol = webtransport'). It has its own
  /// port because the browser only accepts a short-lived certificate there,
  /// and the pinned device identity lives for ten years.
  std::atomic<int> webtransport_port{0};
};

}  // namespace molink
