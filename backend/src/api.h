#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "access_requests.h"
#include "call_hub.h"
#include "server_client.h"
#include "capture/capture.h"
#include "chat_hub.h"
#include "clipboard/clipboard.h"
#include "auth_state.h"
#include "config.h"
#include "input/input.h"
#include "lock_state.h"
#include "network_settings.h"
#include "session_registry.h"

namespace uvcpp {
class uvcpp_web_app;
}

namespace molink {

class discovery_service;
class ui_store;

struct api_deps {
  std::shared_ptr<session_registry> registry;
  std::shared_ptr<capture_source> capture;
  std::shared_ptr<input_injector> input;
  std::shared_ptr<clipboard_service> clipboard;
  std::shared_ptr<lock_state> lock;
  std::shared_ptr<network_settings> network;
  std::string device_id;
  /// SHA-256 fingerprint of the host's self-signed transport certificate.
  std::string cert_fingerprint;
  /// Random token handed only to the desktop shell through the ready line. It
  /// gates loopback web UI access when allow_web_client is off.
  std::string client_token;
  std::shared_ptr<auth_state> auth;
  std::shared_ptr<access_request_registry> access_requests;
  std::shared_ptr<chat_hub> chat;
  std::shared_ptr<call_hub> call;
  std::shared_ptr<server_client> server;
  std::shared_ptr<discovery_service> discovery;
  std::shared_ptr<ui_store> ui;
  std::shared_ptr<std::atomic<int>> bound_port;
  /// True when this build can carry HTTP/2 (h2c) on the web port.
  bool http2_available = false;
  /// Resolved capture tile edge (0 when capture is unavailable).
  int tile_size = 0;
  int change_threshold = 0;
  core_config config;
  uint64_t started_ms = 0;
  /// True when a built frontend is mounted at "/", so the JSON index route is
  /// not registered and the static handler owns that path.
  bool serve_web = false;
};

/// Registers CORS, the bearer-token guard for /api/*, and the JSON endpoints.
/// All routes must be registered before app.start().
void register_http_routes(uvcpp::uvcpp_web_app& app, const api_deps& deps);

}  // namespace molink
