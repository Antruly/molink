#pragma once

#include <string>

namespace molink {

/// State of the optional Windows LocalSystem helper. On other platforms
/// `available` stays false and every action is a no-op.
struct service_status_info {
  bool available = false;
  bool installed = false;
  bool running = false;
  /// "windows" for the session helper, "linux" for the systemd user service.
  std::string platform;
  /// True when the service is a login autostart (systemd), not a background
  /// helper that the core itself starts and stops.
  bool autostart = false;
  /// Windows: the ImagePath the service is actually registered with.
  std::string registered_path;
  /// Windows: where this build expects the helper to live, i.e.
  /// %ProgramData%\moLink\service\molink-service.exe.
  std::string expected_path;
  /// Windows: the registered path differs from `expected_path`, or the binary
  /// it names is gone. Installed-only: a missing service is not "stale".
  bool stale = false;
  /// Windows: the core process already holds administrator rights, so
  /// ShellExecuteW("runas", ...) will not show a UAC prompt.
  bool elevated = false;
  std::string error;
};

service_status_info query_session_service();

/// Pure rule behind `service_status_info::stale`. Paths are compared
/// case-insensitively (Windows semantics) after stripping surrounding quotes.
/// An empty `expected` means "cannot tell", so it is never stale; an empty
/// registered path is stale because it names no binary at all.
bool service_status_is_stale(const std::string& registered,
                             const std::string& expected,
                             bool registered_exists);
/// True when the core itself runs in the physical console session, so the
/// SYSTEM helper (launched there) captures the same desktop. RDP sessions
/// keep using DXGI for their own desktop.
bool running_in_console_session();
/// Start/stop the service. The installer grants the interactive user these
/// rights; no elevation prompt is expected after install.
bool start_session_service(std::string& err);
bool stop_session_service(std::string& err);
bool restart_session_service(std::string& err);
/// Generate the Secure Attention Sequence (Ctrl+Alt+Del). Windows only; the
/// SYSTEM service performs it because a normal user process cannot.
bool send_secure_attention(std::string& err);
/// Run `molink-service.exe --install` / `--uninstall` with UAC. Returns false
/// when the helper is missing or the user cancels the prompt.
bool install_session_service(std::string& err);
bool uninstall_session_service(std::string& err);

}  // namespace molink
