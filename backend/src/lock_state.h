#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace molink {

/// UI access gate shared by the HTTP handlers and the WebSocket.
///
/// The local UI is trusted (loopback), but it locks after an idle period so
/// someone walking up to the machine cannot use it. A remote browser must
/// always authenticate with the verification code, whether or not the local
/// UI is locked.
class lock_state {
 public:
  explicit lock_state(int timeout_seconds);

  bool locked() const;
  int timeout_seconds() const;
  void set_timeout_seconds(int seconds);

  /// Sessions the UI currently has open (to other devices or this host).
  /// The core cannot see those outbound sockets, so each UI window reports its
  /// own count, keyed by a per-window client id; entries expire when a window
  /// disappears without sending a final zero.
  int open_sessions() const;
  void set_ui_sessions(const std::string& client, int count);

  /// Records user activity. Background polling must not call this.
  void touch();

  bool should_lock(uint64_t now, bool has_authenticated_sessions) const;
  void lock();

  /// Validates the verification code and clears the lock. Failed attempts are
  /// rate limited so the finite UI can not be brute forced through the lock
  /// screen.
  bool try_unlock(const std::string& supplied, const std::string& expected);

 private:
  mutable std::mutex mutex_;
  bool locked_ = false;
  int timeout_seconds_ = 60;
  struct ui_client {
    int sessions = 0;
    uint64_t updated_ms = 0;
  };
  mutable std::map<std::string, ui_client> ui_clients_;
  uint64_t last_activity_ms_ = 0;
  int failed_attempts_ = 0;
  uint64_t blocked_until_ms_ = 0;

  int total_sessions_locked(uint64_t now) const;
};

}  // namespace molink
