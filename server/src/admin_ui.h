#pragma once

#include <string>

#include <webapp/uvcpp_web_app.h>

#include "config.h"

namespace molink {

/// Resolve the built admin SPA directory. Empty when no index.html is found.
std::string resolve_admin_ui_root(const server_config& config);

/// Serve the SPA at / (and hashed assets at /assets), or a JSON explanation
/// when the build is not staged.
void register_admin_ui(uvcpp::uvcpp_web_app& app, const std::string& root);

}  // namespace molink
