// Windows service control for the host-only build.
//
// The product owns a dedicated service name, "moLinkHost", so it can be
// installed next to the full product's "moLinkSession" without the two UIs
// reporting each other's state. Query/start/stop go straight to the service
// control manager; install/uninstall run the existing molink-service.exe
// helper with a service-name argument.
#pragma once

#include <string>

namespace molink {
namespace hostui {

enum class service_state {
  unknown,
  not_installed,
  stopped,
  start_pending,
  stop_pending,
  running,
  paused,
  other,
};

struct service_status {
  service_state state = service_state::unknown;
  bool installed = false;
  bool running = false;
  std::string error;
};

/// Chinese label for the status line.
const char* service_state_text(service_state state);

/// Query "moLinkHost" from the SCM. On non-Windows hosts this reports
/// unknown/not installed so the rest of the shell still compiles and runs.
service_status query_host_service();

bool start_host_service(std::string& error);
bool stop_host_service(std::string& error);

/// Run `molink-service.exe --install --name moLinkHost` (elevated) and verify
/// the service exists afterwards. Uninstall is idempotent: deleting a service
/// that is already gone succeeds.
bool install_host_service(std::string& error);
bool uninstall_host_service(std::string& error);

}  // namespace hostui
}  // namespace molink
