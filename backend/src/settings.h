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

  /// Master switch for the remote directory. Off means LAN/direct only.
  bool server_enabled = true;
  /// Built-in directory is used when this is empty (and the switch is on).
  std::string server_url;
  std::string server_invite;
  /// Optional PEM CA bundle used to verify an HTTPS directory.
  std::string server_ca;
  /// "preset:<name>" or a small image data URL, synced to the directory.
  std::string avatar;

  /// Long-term code: at least 8 characters and must mix letters and digits.
  std::string long_term_code;

  /// Temporary code shown on the home page.
  std::string temp_mode = "single";  // "single" | "multi"
  std::string temp_code;
  int temp_uses = 1;
  int temp_remaining = 1;
};

app_settings load_settings();
void save_settings(const app_settings& settings);

/// Human-readable default name for this machine.
std::string default_device_name();

/// The effective directory URL: empty when disabled, otherwise the custom URL
/// or the built-in default.
std::string effective_server_url(const app_settings& settings);
std::string default_server_url();

/// CA bundle for the effective directory. A user-provided server_ca wins. For
/// the built-in directory there is no public CA to rely on: its certificate is
/// issued by Cloudflare's Origin CA, so the bundled root is written to the
/// config directory and used by default. Custom directories and an empty
/// server_ca keep the system trust store.
std::string effective_server_ca(const app_settings& settings);

}  // namespace molink
