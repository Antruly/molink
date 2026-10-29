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
  /// Encrypt sessions that arrive through the directory/ID server. On by
  /// default; the relay already uses WSS and the cores add an end-to-end layer.
  bool encrypt_remote = true;
  /// Encrypt direct LAN sessions. Off by default so a LAN connection does not
  /// pop a self-signed certificate prompt unless the host opts in.
  bool encrypt_lan = false;
  /// User preference for the optional Windows service. Persisted so a manual
  /// stop survives restarts; an overwrite install must not force it on.
  bool service_desired_running = true;
  std::string device_name;

  /// Host-side AV1 encoding. When on, the capture thread encodes the changed
  /// picture as AV1 and the page paints the core's decode of it.
  bool av1_enabled = true;
  /// True: AV1 is the only picture path, so every changed rectangle is AV1 and
  /// the tile payload is left empty. Tiles are then a pure fallback for
  /// clients that cannot decode AV1. False keeps the older split, where only
  /// large rectangles become AV1 patches on top of a tile frame.
  bool av1_full_frame = true;
  /// "auto" or an id reported by av1_encoder_devices(): "gpu-nvenc" when an
  /// AV1-capable NVIDIA driver is present, "cpu-svt" otherwise.
  std::string av1_device = "auto";
  /// "auto" | "fast" | "balanced" | "quality". Auto scales the SVT-AV1 preset
  /// with the detected logical processors.
  std::string av1_speed = "auto";
  /// 0 = follow the quality preset, otherwise 1..63 (lower is better).
  int av1_qp = 0;
  /// 0 = follow the quality preset, otherwise 1..100 (% of the screen).
  int av1_max_area_percent = 0;
  /// 0 = derive from the CPU, otherwise 1..64 encoder threads.
  int av1_threads = 0;

  /// Host capture keyframe interval in seconds. 0 = off; the built-in 30 s
  /// safety refresh still runs. 1..60 forces a full keyframe at that cadence
  /// (1 s is the most real-time, at a large bandwidth cost).
  int keyframe_interval_seconds = 0;

  /// Master switch for the remote directory. Off means LAN/direct only.
  bool server_enabled = true;
  /// Built-in directory is used when this is empty (and the switch is on).
  std::string server_url;
  std::string server_invite;
  /// Optional PEM CA bundle used to verify an HTTPS directory.
  std::string server_ca;
  /// STUN server for ICE: "host[:port]", empty = built-in default,
  /// "none" disables server-reflexive candidates.
  std::string stun_server;
  /// "preset:<name>" or a small image data URL, synced to the directory.
  std::string avatar;

  /// Long-term code: at least 8 characters and must mix letters and digits.
  std::string long_term_code;

  /// Temporary code shown on the home page.
  std::string temp_mode = "single";  // "single" | "multi"
  std::string temp_code;
  int temp_uses = 1;
  int temp_remaining = 1;
  /// Each temporary mode can be switched off. The single-use mode may only be
  /// off while a long-term code exists, so a host cannot lock itself out.
  bool single_code_enabled = true;
  bool multi_code_enabled = true;
  /// Epoch day the temporary code was last rotated on (daily rotation).
  long long temp_rotated_day = 0;
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
