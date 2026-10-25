#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace molink {

/// UI access gate shared by the HTTP handlers and the WebSocket.
///
/// The local UI is trusted (loopback) and never locks: whoever is sitting at
/// the machine is already allowed to use it. Only remote browsers lock after
/// an idle period, so an unattended tab on another machine asks for the
/// verification code again. Loopback requests are exempt from the lock gate
/// entirely, and their activity does not keep a remote session unlocked.
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
  void set_ui_sessions(const std::string& client, int count, bool remote);

  /// Notes that a non-loopback client is connected. This does not reset the
  /// idle timer; background polling must not call this either.
  void note_remote();

  /// Records user activity. Only non-loopback activity resets the idle timer;
  /// a local UI must not keep a remote tab unlocked. Background polling must
  /// not call this.
  void touch(bool remote);

  bool should_lock(uint64_t now, bool has_authenticated_sessions) const;
  void lock();

  /// Clears the lock after the caller has verified a code or session token.
  void unlock();

  /// Per-IP authentication budget. Returns 0 when @p peer_ip may try now, a
  /// positive number of seconds to wait while temporarily blocked, or -1 when
  /// the address has been permanently banned. Loopback is always allowed.
  int attempt_delay(const std::string& peer_ip) const;

  /// Records one failed attempt and returns the same encoding as
  /// attempt_delay(). The fifth failure starts a lockout; later lockouts grow
  /// (30 s, 2 min, 10 min, 1 h, 6 h) and the sixth round bans the address.
  int record_failure(const std::string& peer_ip);

  /// Clears the failure/escalation state after a successful authentication.
  void record_success(const std::string& peer_ip);

 private:
  mutable std::mutex mutex_;
  bool locked_ = false;
  int timeout_seconds_ = 60;
  struct ui_client {
    int sessions = 0;
    bool remote = false;
    uint64_t updated_ms = 0;
  };
  mutable std::map<std::string, ui_client> ui_clients_;
  /// Set once a non-loopback client has been seen. Without it a server that
  /// only ever sees the local shell would lock itself with nothing to lock.
  bool remote_seen_ = false;
  /// Last user activity reported by a non-loopback client. Local activity is
  /// deliberately not recorded here.
  uint64_t last_remote_activity_ms_ = 0;

  struct attempt_record {
    int failures = 0;
    int strikes = 0;
    uint64_t last_failure_ms = 0;
    uint64_t blocked_until_ms = 0;
  };
  std::map<std::string, attempt_record> attempts_;
  std::set<std::string> banned_;

  static std::string normalize_peer(const std::string& peer);
  void save_bans() const;

  int total_sessions_locked(uint64_t now) const;
  int remote_sessions_locked(uint64_t now) const;
};

}  // namespace molink
