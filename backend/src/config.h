#pragma once

#include <string>

namespace molink {

struct core_config {
  std::string host = "127.0.0.1";
  int port = 21118;
  /// Shared-secret required by /api/* and by the WebSocket control handshake.
  /// Empty means "generate one at startup" (the default).
  std::string token;
  bool print_ready_json = false;
  bool allow_remote = false;
  bool show_help = false;
  bool show_version = false;
  /// When the shell supervises this process, it passes its own pid so the core
  /// can exit if the shell dies without cleaning up. 0 disables the watch.
  long parent_pid = 0;
  /// Screen capture rate. Dirty tiles are computed per captured frame.
  int capture_fps = 15;
  /// Force the synthetic source even when a display is available.
  bool no_capture = false;
};

/// Parse command-line arguments. Unknown arguments are reported through
/// @p err and make the function return false.
bool parse_args(int argc, char** argv, core_config& cfg, std::string& err);

/// 32 lowercase hex characters from the system CSPRNG.
std::string generate_token();

/// Compare two secrets without an early exit on the first differing byte.
bool constant_time_equals(const std::string& a, const std::string& b);

}  // namespace molink
