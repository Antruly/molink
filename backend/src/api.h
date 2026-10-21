#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "capture/capture.h"
#include "config.h"
#include "session_registry.h"

namespace uvcpp {
class uvcpp_web_app;
}

namespace molink {

struct api_deps {
  std::shared_ptr<session_registry> registry;
  std::shared_ptr<capture_source> capture;
  core_config config;
  uint64_t started_ms = 0;
};

/// Registers CORS, the bearer-token guard for /api/*, and the JSON endpoints.
/// All routes must be registered before app.start().
void register_http_routes(uvcpp::uvcpp_web_app& app, const api_deps& deps);

}  // namespace molink
