#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "capture/capture.h"
#include "clipboard/clipboard.h"
#include "config.h"
#include "input/input.h"
#include "lock_state.h"
#include "session_registry.h"

namespace uvcpp {
class uvcpp_web_app;
}

namespace molink {

struct api_deps {
  std::shared_ptr<session_registry> registry;
  std::shared_ptr<capture_source> capture;
  std::shared_ptr<input_injector> input;
  std::shared_ptr<clipboard_service> clipboard;
  std::shared_ptr<lock_state> lock;
  std::string device_id;
  std::shared_ptr<std::atomic<int>> bound_port;
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
