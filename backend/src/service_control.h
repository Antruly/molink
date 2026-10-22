#pragma once

#include <string>

namespace molink {

/// State of the optional Windows LocalSystem helper. On other platforms
/// `available` stays false and every action is a no-op.
struct service_status_info {
  bool available = false;
  bool installed = false;
  bool running = false;
  std::string error;
};

service_status_info query_session_service();
/// True when the core itself runs in the physical console session, so the
/// SYSTEM helper (launched there) captures the same desktop. RDP sessions
/// keep using DXGI for their own desktop.
bool running_in_console_session();
/// Start/stop the service. The installer grants the interactive user these
/// rights; no elevation prompt is expected after install.
bool start_session_service(std::string& err);
bool stop_session_service(std::string& err);
/// Run `molink-service.exe --install` / `--uninstall` with UAC. Returns false
/// when the helper is missing or the user cancels the prompt.
bool install_session_service(std::string& err);
bool uninstall_session_service(std::string& err);

}  // namespace molink
