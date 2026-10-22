#include "access_requests.h"

#include <algorithm>
#include <utility>

namespace molink {

uint32_t access_request_registry::add(const access_request_info& info,
                                      std::function<void()> approve,
                                      std::function<void()> deny) {
  std::lock_guard<std::mutex> lock(mutex_);
  // One outstanding request per peer: a retry should not fill the queue.
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->second.info.peer_ip == info.peer_ip) {
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  if (pending_.size() >= kMaxPending) return 0;
  const uint32_t id = next_id_++;
  if (next_id_ == 0) next_id_ = 1;
  entry item;
  item.info = info;
  item.info.id = id;
  item.approve = std::move(approve);
  item.deny = std::move(deny);
  pending_.emplace(id, std::move(item));
  return id;
}

std::vector<access_request_info> access_request_registry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<access_request_info> out;
  out.reserve(pending_.size());
  for (const auto& [id, item] : pending_) {
    (void)id;
    out.push_back(item.info);
  }
  return out;
}

bool access_request_registry::approve(uint32_t id) {
  std::function<void()> callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(id);
    if (it == pending_.end()) return false;
    callback = std::move(it->second.approve);
    pending_.erase(it);
  }
  if (callback) callback();
  return true;
}

bool access_request_registry::deny(uint32_t id) {
  std::function<void()> callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(id);
    if (it == pending_.end()) return false;
    callback = std::move(it->second.deny);
    pending_.erase(it);
  }
  if (callback) callback();
  return true;
}

void access_request_registry::remove(uint32_t id) {
  if (id == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  pending_.erase(id);
}

}  // namespace molink
