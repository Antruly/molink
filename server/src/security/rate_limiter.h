#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace molink {

/// Token buckets keyed by scope + client. The clock is passed in so tests can
/// advance time without sleeping.
class rate_limiter {
 public:
  explicit rate_limiter(std::size_t max_keys = 100000);

  /// Consume one token. `per_second` is the refill rate, `burst` the bucket
  /// capacity. On rejection, `retry_after_seconds` is the wait until one token
  /// is available.
  bool allow(const std::string& key, double per_second, double burst,
             std::uint64_t now_ms, int* retry_after_seconds = nullptr);

  /// Drop buckets that have been idle for more than ten minutes.
  void prune(std::uint64_t now_ms);
  std::size_t size() const;

 private:
  struct bucket {
    double tokens = 0.0;
    std::uint64_t last_ms = 0;
    std::uint64_t used_ms = 0;
  };

  void evict_locked(std::uint64_t now_ms);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, bucket> buckets_;
  std::size_t max_keys_;
};

}  // namespace molink
