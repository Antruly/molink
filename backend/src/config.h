#pragma once

#include <string>

#include "network_settings.h"

namespace molink {

struct core_config {
  /// The listener binds all interfaces by default so the Security toggle can
  /// take effect without a restart. Non-loopback TCP connections are closed
  /// before HTTP parsing unless allow_remote_access is on. An explicit
  /// --host 127.0.0.1 truly binds local-only.
  std::string host = "0.0.0.0";
  bool host_explicit = false;
  int port = kDefaultTcpPort;
  bool port_explicit = false;
  int http3_port = kDefaultHttp3Port;
  bool http3_port_explicit = false;
  int quic_port = kDefaultQuicPort;
  bool quic_port_explicit = false;
  int tcp_data_port = kDefaultTcpDataPort;
  bool tcp_data_port_explicit = false;
  int udp_port = kDefaultUdpPort;
  bool udp_port_explicit = false;
  /// Shared-secret required by /api/* and by the WebSocket control handshake.
  /// Empty means "generate one at startup" (the default).
  std::string token;
  bool print_ready_json = false;
  /// Required only when --host explicitly names a non-loopback address.
  bool allow_remote = false;
  bool show_help = false;
  bool show_version = false;
  /// When the shell supervises this process, it passes its own pid so the core
  /// can exit if the shell dies without cleaning up. 0 disables the watch.
  long parent_pid = 0;
  /// Screen capture rate. Dirty tiles are computed per captured frame.
  int capture_fps = 15;
  /// Tile edge in pixels. 0 = choose from the detected CPU capability.
  int tile_size = 0;
  /// Per-channel difference that still counts as "unchanged" (0 = exact).
  /// Ignoring imperceptible noise cuts frames per second without visible loss.
  int change_threshold = 4;
  /// Force the synthetic source even when a display is available.
  bool no_capture = false;
  /// Disable pointer/keyboard injection into the local display.
  bool no_input = false;
  /// Disable zlib tile compression.
  bool no_compress = false;
  /// Disable clipboard synchronization.
  bool no_clipboard = false;
  /// Directory served at "/" (the built frontend). Empty = auto-detect.
  std::string web_root;
  /// Idle seconds before the UI locks; 0 disables. Default 60.
  int lock_timeout_seconds = 60;
  /// True when --lock-timeout was given, so a persisted UI setting is ignored.
  bool lock_timeout_explicit = false;
};

/// Parse command-line arguments. Unknown arguments are reported through
/// @p err and make the function return false.
bool parse_args(int argc, char** argv, core_config& cfg, std::string& err);

/// 32 lowercase hex characters from the system CSPRNG.
std::string generate_token();

/// Compare two secrets without an early exit on the first differing byte.
bool constant_time_equals(const std::string& a, const std::string& b);

}  // namespace molink
