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
  /// Per-path transport encryption: directory/ID sessions and direct LAN
  /// sessions have separate defaults (remote on, LAN off).
  void set_transport_security(bool encrypt_remote, bool encrypt_lan);
  void set_service_desired_running(bool enabled);
  /// Pinned stream codec: auto / av1 / h264 / vp9 / vp8.
  bool set_stream_codec(const std::string& codec);
  /// Video path: stream / mixed / tiles.
  bool set_video_mode(const std::string& mode);
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

  mutable std::mutex mutex_;
  app_settings settings_;
  std::string local_session_;
  /// Session tokens and when they were issued (monotonic ms).
  std::deque<std::pair<std::string, uint64_t>> remote_sessions_;
};

}  // namespace molink
