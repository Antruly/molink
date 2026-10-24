#include "security/rate_limiter.h"

#include <algorithm>
#include <cmath>

namespace molink {

namespace {
constexpr std::uint64_t kIdlePruneMs = 10u * 60u * 1000u;
}

rate_limiter::rate_limiter(std::size_t max_keys)
    : max_keys_(max_keys == 0 ? 1 : max_keys) {}

bool rate_limiter::allow(const std::string& key, double per_second,
                         double burst, std::uint64_t now_ms,
                         int* retry_after_seconds) {
  if (retry_after_seconds != nullptr) *retry_after_seconds = 0;
  if (per_second <= 0.0 || burst <= 0.0) return false;
  const double capacity = std::max(1.0, burst);

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = buckets_.find(key);
  if (it == buckets_.end()) {
    if (buckets_.size() >= max_keys_) evict_locked(now_ms);
    bucket fresh;
    fresh.tokens = capacity;
    fresh.last_ms = now_ms;
    fresh.used_ms = now_ms;
    it = buckets_.emplace(key, fresh).first;
  }
  bucket& state = it->second;
  if (now_ms > state.last_ms) {
    const double seconds = static_cast<double>(now_ms - state.last_ms) / 1000.0;
    state.tokens = std::min(capacity, state.tokens + seconds * per_second);
    state.last_ms = now_ms;
  }
  state.used_ms = now_ms;

  if (state.tokens >= 1.0) {
    state.tokens -= 1.0;
    return true;
  }
  const double missing = 1.0 - state.tokens;
  const double wait = missing / per_second;
  if (retry_after_seconds != nullptr) {
    *retry_after_seconds = static_cast<int>(std::ceil(std::max(1.0, wait)));
  }
  return false;
}

void rate_limiter::prune(std::uint64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = buckets_.begin(); it != buckets_.end();) {
    if (now_ms > it->second.used_ms + kIdlePruneMs) {
      it = buckets_.erase(it);
    } else {
      ++it;
    }
  }
}

std::size_t rate_limiter::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return buckets_.size();
}

void rate_limiter::evict_locked(std::uint64_t now_ms) {
  for (auto it = buckets_.begin(); it != buckets_.end();) {
    if (now_ms > it->second.used_ms + kIdlePruneMs) {
      it = buckets_.erase(it);
    } else {
      ++it;
    }
  }
  while (buckets_.size() >= max_keys_) {
    // Arbitrary but bounded: drop the least recently used bucket. A scan at
    // this size is rare (only when the map is full) and keeps the code small.
    auto oldest = buckets_.begin();
    for (auto it = buckets_.begin(); it != buckets_.end(); ++it) {
      if (it->second.used_ms < oldest->second.used_ms) oldest = it;
    }
    buckets_.erase(oldest);
  }
}

}  // namespace molink
