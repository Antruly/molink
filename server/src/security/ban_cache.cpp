#include "security/ban_cache.h"

#include <algorithm>

namespace molink {

bool ban_cache::load(uvcpp::uvcpp_db_client& client,
                     const sql_dialect& dialect, ban_repository& repository,
                     std::uint64_t now_ms, std::string& error) {
  std::vector<ban_record> items;
  std::size_t total = 0;
  if (!repository.list(client, dialect, 0, 10000, items, total, error)) {
    return false;
  }
  std::vector<ban_record> active;
  active.reserve(items.size());
  for (ban_record& ban : items) {
    if (ban.expires_ms != 0 && ban.expires_ms <= now_ms) continue;
    active.push_back(std::move(ban));
  }
  std::lock_guard<std::mutex> lock(mutex_);
  bans_ = std::move(active);
  return true;
}

bool ban_cache::blocked(const ip_addr& address, std::uint64_t now_ms,
                        ban_record& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const ban_record& ban : bans_) {
    if (ban.expires_ms != 0 && ban.expires_ms <= now_ms) continue;
    if (ban.target.find('/') != std::string::npos) {
      cidr_block block;
      if (parse_cidr(ban.target, block, nullptr) &&
          cidr_contains(block, address)) {
        out = ban;
        return true;
      }
    } else {
      ip_addr target;
      if (parse_ip(ban.target, target, nullptr) &&
          canonical_ip(target) == canonical_ip(address)) {
        out = ban;
        return true;
      }
    }
  }
  return false;
}

void ban_cache::add(const ban_record& ban) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const ban_record& existing : bans_) {
    if (existing.id == ban.id) return;
  }
  bans_.push_back(ban);
}

void ban_cache::remove(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  bans_.erase(std::remove_if(bans_.begin(), bans_.end(),
                             [&](const ban_record& ban) {
                               return ban.id == id;
                             }),
              bans_.end());
}

std::size_t ban_cache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bans_.size();
}

}  // namespace molink
