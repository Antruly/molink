#pragma once

#include <string>

#include "config.h"

namespace molink {

/// Windows SCM helpers. On POSIX these report that service mode is
/// unavailable; systemd/launchd run the console binary directly.
bool install_service(const server_config& config, std::string& error);
bool uninstall_service(const server_config& config, std::string& error);

/// Blocks while the service runs. `config` must already include the resolved
/// config path and data directory.
int run_service(server_config config);

}  // namespace molink
