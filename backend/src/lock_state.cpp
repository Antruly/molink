#include "lock_state.h"

#include <vector>

#include "config.h"
#include "identity.h"
#include "util.h"

namespace molink {
namespace {

constexpr int kFailureLimit = 5;
// Each completed round of kFailureLimit failures escalates one step. After the
// last duration the next round bans the address outright.
constexpr int kLockoutSeconds[] = {30, 120, 600, 3600, 21600};
constexpr int kLockoutSteps =
    static_cast<int>(sizeof(kLockoutSeconds) / sizeof(kLockoutSeconds[0]));
constexpr int kBanishAfterStrikes = kLockoutSteps + 1;
// A quiet day resets the escalation ladder, so occasional typos do not add up
// to a permanent ban. The ban itself never expires.
constexpr uint64_t kStrikeResetMs = 24ull * 60 * 60 * 1000;

}  // namespace

lock_state::lock_state(int timeout_seconds)
    : timeout_seconds_(timeout_seconds < 0 ? 0 : timeout_seconds),
      last_activity_ms_(now_ms()) {
  for (const std::string& ip : load_banned_ips()) {
    const std::string normalized = normalize_peer(ip);
    if (!normalized.empty()) banned_.insert(normalized);
  }
}

bool lock_state::locked() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return locked_;
}

int lock_state::timeout_seconds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return timeout_seconds_;
}

void lock_state::set_timeout_seconds(int seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  timeout_seconds_ = seconds < 0 ? 0 : seconds;
  last_activity_ms_ = now_ms();
}

int lock_state::total_sessions_locked(uint64_t now) const {
  int total = 0;
  for (auto it = ui_clients_.begin(); it != ui_clients_.end();) {
    if (now > it->second.updated_ms &&
        now - it->second.updated_ms > 60000) {
      it = ui_clients_.erase(it);
      continue;
    }
    total += it->second.sessions;
    ++it;
  }
  return total;
}

int lock_state::open_sessions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_sessions_locked(now_ms());
}

void lock_state::set_ui_sessions(const std::string& client, int count) {
  std::lock_guard<std::mutex> lock(mutex_);
  ui_client& entry = ui_clients_[client.empty() ? "default" : client];
  entry.sessions = count < 0 ? 0 : (count > 64 ? 64 : count);
  entry.updated_ms = now_ms();
  last_activity_ms_ = entry.updated_ms;
}

void lock_state::touch() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_activity_ms_ = now_ms();
}

bool lock_state::should_lock(uint64_t now,
                             bool has_authenticated_sessions) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (locked_ || timeout_seconds_ == 0 || has_authenticated_sessions ||
      total_sessions_locked(now) > 0) {
    return false;
  }
  return now > last_activity_ms_ &&
         now - last_activity_ms_ >=
             static_cast<uint64_t>(timeout_seconds_) * 1000u;
}

void lock_state::lock() {
  std::lock_guard<std::mutex> lock(mutex_);
  locked_ = true;
  last_activity_ms_ = now_ms();
}

void lock_state::unlock() {
  std::lock_guard<std::mutex> lock(mutex_);
  locked_ = false;
  last_activity_ms_ = now_ms();
}

std::string lock_state::normalize_peer(const std::string& peer) {
  if (peer.compare(0, 7, "::ffff:") == 0) return peer.substr(7);
  return peer;
}

int lock_state::attempt_delay(const std::string& peer_ip) const {
  const std::string ip = normalize_peer(peer_ip);
  if (ip.empty() || is_loopback_peer(ip)) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (banned_.count(ip) != 0) return -1;
  const auto it = attempts_.find(ip);
  if (it == attempts_.end()) return 0;
  const uint64_t now = now_ms();
  if (now >= it->second.blocked_until_ms) return 0;
  const uint64_t remaining = it->second.blocked_until_ms - now;
  const uint64_t seconds = (remaining + 999) / 1000;
  return seconds > 86400 ? 86400 : static_cast<int>(seconds);
}

int lock_state::record_failure(const std::string& peer_ip) {
  const std::string ip = normalize_peer(peer_ip);
  if (ip.empty() || is_loopback_peer(ip)) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (banned_.count(ip) != 0) return -1;

  const uint64_t now = now_ms();
  attempt_record& record = attempts_[ip];
  if (record.last_failure_ms != 0 && now > record.last_failure_ms &&
      now - record.last_failure_ms > kStrikeResetMs) {
    record = attempt_record{};
  }
  if (now < record.blocked_until_ms) {
    const uint64_t remaining = record.blocked_until_ms - now;
    const uint64_t seconds = (remaining + 999) / 1000;
    return seconds > 86400 ? 86400 : static_cast<int>(seconds);
  }

  record.last_failure_ms = now;
  ++record.failures;
  if (record.failures < kFailureLimit) return 0;

  record.failures = 0;
  ++record.strikes;
  if (record.strikes >= kBanishAfterStrikes) {
    banned_.insert(ip);
    attempts_.erase(ip);
    save_bans();
    return -1;
  }
  const int step = record.strikes - 1;
  const int seconds = kLockoutSeconds[step < kLockoutSteps ? step : kLockoutSteps - 1];
  record.blocked_until_ms = now + static_cast<uint64_t>(seconds) * 1000;
  return seconds;
}

void lock_state::record_success(const std::string& peer_ip) {
  const std::string ip = normalize_peer(peer_ip);
  if (ip.empty() || is_loopback_peer(ip)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  attempts_.erase(ip);
}

void lock_state::save_bans() const {
  std::vector<std::string> ips(banned_.begin(), banned_.end());
  save_banned_ips(ips);
}

}  // namespace molink
