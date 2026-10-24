#pragma once

#include <string>

namespace molink {

/// User-visible security and identity settings. Secrets live in this file
/// (0600) until the SQLite/OS-keychain migration; it is never served to a
/// remote authenticated peer.
struct app_settings {
  /// Bind 0.0.0.0 and accept non-loopback peers. Off by default.
  bool allow_remote_access = false;
  /// Allow a normal browser (not the desktop shell) to load the web UI.
  bool allow_web_client = false;
  /// Multicast LAN discovery. Requires allow_remote_access.
  bool discovery_enabled = false;
  /// Let a remote peer ask the local user for access when it has no code.
  /// The local user still has to approve every request.
  bool allow_access_requests = true;
  /// User preference for the optional Windows service. Persisted so a manual
  /// stop survives restarts; an overwrite install must not force it on.
  bool service_desired_running = true;
  std::string device_name;

  /// Phase-2 directory server. Empty URL disables registration.
  std::string server_url;
  std::string server_invite;
  /// Optional PEM CA bundle used to verify an HTTPS directory.
  std::string server_ca;

  /// Long-term code: at least 8 characters and must mix letters and digits.
  std::string long_term_code;

  /// Temporary code shown on the home page.
  std::string temp_mode = "multi";  // "single" | "multi"
  std::string temp_code;
  int temp_uses = 20;
  int temp_remaining = 20;
};

app_settings load_settings();
void save_settings(const app_settings& settings);

/// Human-readable default name for this machine.
std::string default_device_name();

}  // namespace molink
