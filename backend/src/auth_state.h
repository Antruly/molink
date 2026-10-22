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
  void update_access(bool allow_remote_access, bool allow_web_client,
                     bool discovery_enabled, const std::string& device_name);
  /// Toggle the no-code approval flow without touching the other access
  /// switches. Defaults to enabled.
  void set_allow_access_requests(bool allow);

 private:
  bool valid_session_locked(const std::string& token) const;
  bool verify_code_locked(const std::string& supplied, bool consume);
  void rotate_temp_locked();
  void invalidate_remote_sessions_locked();
  void persist_locked();

  mutable std::mutex mutex_;
  app_settings settings_;
  std::string local_session_;
  std::deque<std::string> remote_sessions_;
};

}  // namespace molink
