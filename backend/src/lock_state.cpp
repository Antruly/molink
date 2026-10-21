#include "lock_state.h"

#include "config.h"
#include "util.h"

namespace molink {

lock_state::lock_state(int timeout_seconds)
    : timeout_seconds_(timeout_seconds < 0 ? 0 : timeout_seconds),
      last_activity_ms_(now_ms()) {}

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

bool lock_state::try_unlock(const std::string& supplied,
                            const std::string& expected) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t now = now_ms();
  if (now < blocked_until_ms_) return false;
  if (!constant_time_equals(supplied, expected)) {
    ++failed_attempts_;
    if (failed_attempts_ >= 5) {
      failed_attempts_ = 0;
      blocked_until_ms_ = now + 30000;
    }
    return false;
  }
  locked_ = false;
  failed_attempts_ = 0;
  blocked_until_ms_ = 0;
  last_activity_ms_ = now;
  return true;
}

}  // namespace molink
