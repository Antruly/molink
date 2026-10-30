// Session wiring for the native shell: connects the WS client to the local
// core, sends hello, and feeds the toolbar's session_stats from the traffic.
#pragma once

#include <cstdint>
#include <string>

#include "ui_kit.h"
#include "ws_client.h"

namespace molink::net {

struct session_endpoint {
  std::string host;      ///< remote host (for /ws/direct) or empty for a target
  int port = 0;
  std::string target;    ///< device id for /ws/connect, empty for a typed address
  std::string token;     ///< client_token from the core's ready line
  /// Smoke/testing: talk to the host core directly (/ws/session) instead of
  /// going through a local core's proxy. `token` is then the access code.
  bool direct_session = false;
};

class session_client {
 public:
  /// Opens the session through the local core at `core_port`.
  bool start(const std::string& core_host, int core_port,
             const session_endpoint& endpoint, ui::session_stats* stats,
             std::string* error);
  /// Sends a ping when the interval elapsed; call from the UI timer.
  void tick(uint64_t now_ms);
  void stop();
  bool live() const { return ws_.is_open(); }
  /// Forwards a quality change (mode/preset plus the slider values).
  void send_quality(const std::string& mode, int fps, int qp, int bitrate_kbps,
                    const std::string& rate_mode, const std::string& format);

 private:
  void handle_text(const std::string& text);
  static bool find_number(const std::string& json, const char* key, int* out);
  static bool find_string(const std::string& json, const char* key,
                          std::string* out);

  ws_client ws_;
  ui::session_stats* stats_ = nullptr;
  uint64_t last_ping_ = 0;
  uint64_t ping_sent_at_ = 0;
};

}  // namespace molink::net
