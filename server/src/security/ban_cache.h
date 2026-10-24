#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>

#include "db/repositories.h"
#include "security/ip.h"

namespace molink {

/// In-memory copy of the ban table. Device requests check this on every call,
/// so a ban lookup never touches the database; the admin API and the login
/// guard update both the table and this cache.
class ban_cache {
 public:
  bool load(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
            ban_repository& repository, std::uint64_t now_ms,
            std::string& error);

  bool blocked(const ip_addr& address, std::uint64_t now_ms,
               ban_record& out) const;
  void add(const ban_record& ban);
  void remove(const std::string& id);
  std::size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::vector<ban_record> bans_;
};

}  // namespace molink
