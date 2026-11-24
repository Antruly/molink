#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include "settings.h"

namespace molink {

/// What the home page displays for the temporary verification code.
struct code_view {
  std::string mode;  // "single" | "multi"
  std::string value;
  int remaining = 0;
  int uses = 1;
  bool long_term_set = false;
  /// Whether each temporary mode may be used at all.
  bool single_enabled = true;
  bool multi_enabled = true;
};

/// Two-factor status for the local settings page. The URI is an
/// `otpauth://` document an authenticator app can scan; it is only served to
/// loopback clients.
struct totp_view {
  bool enabled = false;
  std::string uri;
  std::string account;
  std::string issuer = "moLink";
};

/// Outcome of a login attempt. `totp_required` means the first factor was
/// right but the second one has not been supplied; no failure should be
/// counted for that. `totp_invalid` is a wrong six-digit code.
struct login_result {
  bool ok = false;
  bool totp_required = false;
  bool totp_invalid = false;
  /// The supplied value was an existing session token, not a code.
  bool session_token = false;
};

/// Owns the verification codes and the short-lived session tokens issued after
/// a successful login.
///
/// Codes:
///   - long-term: always valid, configured in Security settings, >= 8 chars and
///     must mix letters and digits;
///   - temporary: either single-use or multi-use with a configured budget. A
///     consumed single-use code and an exhausted multi-use code rotate to a
///     fresh code automatically.
///
/// A successful code login returns a random session token. The token guard and
/// WebSocket accept session tokens; codes are only accepted at login/hello, so
/// a temporary code is never replayed as a bearer token on every request.
class auth_state {
 public:
  explicit auth_state(app_settings settings);

  /// Valid session token or a valid code. When @p consume is true and the
  /// supplied value is a temporary code, one use is deducted (and the code may
  /// rotate). Long-term codes are never consumed.
  bool authenticate(const std::string& supplied, bool consume);

  /// Full login path: a session token is accepted as-is, a verification code
  /// additionally needs @p totp when two-factor authentication is enabled.
  /// The temporary code is only consumed once every factor has passed, so a
  /// mistyped six-digit code does not spend a single-use code.
  login_result login(const std::string& supplied, const std::string& totp,
                     bool consume);

  /// Whether a verification code alone is no longer enough.
  bool totp_required() const;
  /// Current two-factor state. When disabled this creates (once) a pending
  /// enrollment secret and returns its otpauth URI.
  totp_view totp_status();
  /// Confirms the pending enrollment with a current six-digit code.
  bool enable_totp(const std::string& code, std::string& err);
  /// Turns two-factor off. @p credential must be a valid long-term or
  /// temporary verification code.
  bool disable_totp(const std::string& credential, std::string& err);

  /// Host-wide default capability policy: full, view_only or assist.
  bool set_access_policy(const std::string& policy);

  bool valid_session(const std::string& token) const;
  /// True when `token` is a session token issued no more than `max_age_ms` ago.
  /// A locked host uses this to let a just-disconnected viewer back in without
  /// demanding a code that a single-use code has already spent.
  bool valid_recent_session(const std::string& token,
                            uint64_t max_age_ms) const;
  /// Marks a session token as just used, so an active viewer keeps its
  /// credential fresh across idle locks.
  void touch_session(const std::string& token);
  std::string create_session();

  /// Stable token handed to the local shell through /api/device.
  std::string local_session_token() const { return local_session_; }

  code_view view() const;
  app_settings settings() const;

  /// Empty @p value generates a strong long-term code. On failure returns an
  /// empty string and fills @p err.
  std::string set_long_term(const std::string& value, std::string& err);
  code_view set_temp(const std::string& mode, int uses,
                     const std::string& value, std::string& err);
  /// Switches the temporary modes on or off. Turning the single-use mode off
  /// requires a long-term code (otherwise the host could lock everyone out).
  code_view set_code_policy(bool single_enabled, bool multi_enabled,
                            std::string& err);
  /// Rotates the temporary code when the calendar day changed. Cheap; safe to
  /// call from request handlers.
  void refresh_daily();
  void update_access(bool allow_remote_access, bool allow_web_client,
                     bool discovery_enabled, const std::string& device_name);
  /// Toggle the no-code approval flow without touching the other access
  /// switches. Defaults to enabled.
  void set_allow_access_requests(bool allow);
  /// Allow a host clipboard change to fan out to every viewer when no viewer
  /// is the active one. Off by default.
  void set_allow_sync_between_sessions(bool allow);
  /// Per-path transport encryption: directory/ID sessions and direct LAN
  /// sessions have separate defaults (remote on, LAN off).
  void set_transport_security(bool encrypt_remote, bool encrypt_lan);
  void set_service_desired_running(bool enabled);
  /// Local diagnostic log recording (the ring buffer stays either way).
  void set_diag_logs_enabled(bool enabled);
  /// Optional write sandbox for the file channel. Off by default; `roots` is
  /// the raw `;`/newline separated list the settings screen edits.
  void set_file_sandbox(bool enabled, std::string roots);
  /// Replacement CIDR allowlist text. The caller has already validated it;
  /// an empty string clears the restriction.
  void set_ip_allowlist(std::string value);
  /// Pinned stream codec: auto / av1 / h264 / vp9 / vp8.
  bool set_stream_codec(const std::string& codec);
  /// Video path: stream / mixed / tiles.
  bool set_video_mode(const std::string& mode);
  /// Toolbar sliders: QP (0 = preset), bitrate (0 = preset), fps (0 = the
  /// separate FPS setting), rate mode ("auto"/"cq"/"cbr") and format
  /// ("" = preset, otherwise gray8/rgb565/rgb24/bgra).
  void set_stream_overrides(int qp, int bitrate_kbps, int fps,
                            const std::string& rate_mode,
                            const std::string& format);
  /// Host AV1 encoder preference. Values are validated by the caller.
  void set_av1_encoding(bool enabled, std::string device, std::string speed,
                        int qp, int max_area_percent, int threads,
                        bool full_frame);
  /// Host capture keyframe cadence in seconds; 0 = off.
  void set_keyframe_interval_seconds(int seconds);
  /// Percentage of the screen that must move before the capture streams.
  void set_stream_enter_percent(int percent);
  /// Phase-2 directory: the switch is independent from the custom URL (an
  /// empty URL with the switch on means the built-in default).
  bool set_server(std::string url, std::string invite, std::string ca,
                  bool enabled, std::string& err);

  /// "preset:<name>" or a small image data URL; empty clears it.
  bool set_avatar(std::string avatar, std::string& err);

  /// STUN server for ICE: "host[:port]", empty = built-in default,
  /// "none" disables STUN.
  bool set_stun_server(const std::string& value);

 private:
  bool valid_session_locked(const std::string& token) const;
  bool verify_code_locked(const std::string& supplied, bool consume);
  void rotate_temp_locked();
  void maybe_rotate_daily_locked();
  bool temp_code_enabled_locked() const;
  code_view view_locked() const;
  void invalidate_remote_sessions_locked();
  void persist_locked();
  /// Session tokens survive a core restart (an update, a service restart):
  /// a remote viewer cannot present a code that a single-use code already
  /// spent, so dropping the tokens on restart locks that viewer out for good.
  void load_sessions_locked();
  void save_sessions_locked() const;
  /// Two-factor state lives beside the settings, encrypted with a local key.
  void load_totp_locked();
  void save_totp_locked() const;
  bool verify_totp_code_locked(const std::string& secret,
                               const std::string& code) const;
  std::string totp_uri_locked(const std::string& secret) const;
  void ensure_pending_totp_locked();
  std::string totp_key_;

  mutable std::mutex mutex_;
  app_settings settings_;
  std::string local_session_;
  /// Session tokens and when they were issued (monotonic ms).
  std::deque<std::pair<std::string, uint64_t>> remote_sessions_;

  /// Raw 20-byte TOTP secret, kept in memory only; at rest it is sealed in
  /// the `2fa` file. `totp_pending_` is the enrollment candidate.
  bool totp_enabled_ = false;
  std::string totp_secret_;
  std::string totp_pending_;
};

}  // namespace molink
