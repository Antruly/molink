#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace molink {

struct access_request_info {
  uint32_t id = 0;
  std::string peer_ip;
  std::string name;
  std::string client;
  uint64_t created_ms = 0;
};

/// Pending "let me in" requests shown to the local user. Approval happens on
/// the app loop through the callbacks the session controller registers.
class access_request_registry {
 public:
  static constexpr std::size_t kMaxPending = 8;

  /// Returns the new request id, or 0 when the queue is full. A second
  /// request from the same peer replaces the earlier one.
  uint32_t add(const access_request_info& info,
               std::function<void()> approve,
               std::function<void()> deny);

  std::vector<access_request_info> list() const;
  bool approve(uint32_t id);
  bool deny(uint32_t id);
  void remove(uint32_t id);

 private:
  struct entry {
    access_request_info info;
    std::function<void()> approve;
    std::function<void()> deny;
  };

  mutable std::mutex mutex_;
  uint32_t next_id_ = 1;
  std::map<uint32_t, entry> pending_;
};

}  // namespace molink
