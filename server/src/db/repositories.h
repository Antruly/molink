#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>

#include "db/records.h"
#include "db/sql_dialect.h"
#include "security/ip.h"

namespace molink {

class device_repository {
 public:
  bool insert(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
              device_record& record, std::string& err) const;
  bool heartbeat(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                 const std::string& id, const std::string& token_hash,
                 const std::string& address, const std::string& peer_ip, int port,
                 const std::string& version, bool& found,
                 std::string& err) const;
  bool remove(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
              const std::string& id, const std::string& token_hash,
              bool& found) const;
  bool revoke(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
              const std::string& id, bool& found) const;
  bool find(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
            const std::string& id, device_record& out, std::string& err) const;
  bool list(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
            const device_query& query, std::uint64_t offline_after_ms,
            std::uint64_t now_ms, device_page& out, std::string& err) const;
  bool count_for_ip(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                    const std::string& peer_ip, std::size_t& out,
                    std::string& err) const;
};

class user_repository {
 public:
  bool create(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
              const admin_user& user, std::string& err) const;
  bool find_by_username(uvcpp::uvcpp_db_client& client,
                        const sql_dialect& dialect,
                        const std::string& username, admin_user& out,
                        bool& found, std::string& err) const;
  bool find_by_id(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                  const std::string& id, admin_user& out, bool& found,
                  std::string& err) const;
  bool list(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
            std::vector<admin_user>& out, std::string& err) const;
  bool update_profile(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                      const std::string& id, const std::string& display_name,
                      const std::string& role, bool disabled,
                      std::string& err) const;
  bool update_password(uvcpp::uvcpp_db_client& client,
                       const sql_dialect& dialect, const std::string& id,
                       const std::string& algo, int iterations,
                       const std::string& salt, const std::string& hash,
                       std::string& err) const;
  bool remove(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
              const std::string& id, bool& found, std::string& err) const;
  bool count_enabled_admins(uvcpp::uvcpp_db_client& client,
                            const sql_dialect& dialect, std::size_t& out,
                            std::string& err) const;
  bool count_all(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                 std::size_t& out, std::string& err) const;
  bool touch_login(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                   const std::string& id, std::uint64_t now_ms) const;
};

class session_repository {
 public:
  bool create(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
              const session_record& session, std::string& err) const;
  bool find_by_token(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                     const std::string& token_sha256, session_record& out,
                     bool& found, std::string& err) const;
  bool touch(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
             const std::string& id, std::uint64_t seen_ms,
             std::uint64_t expires_ms) const;
  bool remove(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
              const std::string& id) const;
  bool remove_for_user(uvcpp::uvcpp_db_client& client,
                       const sql_dialect& dialect, const std::string& user_id,
                       const std::string& keep_id) const;
  bool list_for_user(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                     const std::string& user_id,
                     std::vector<session_record>& out, std::string& err) const;
  bool prune_expired(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                     std::uint64_t now_ms) const;
};

class settings_repository {
 public:
  bool ensure_defaults(uvcpp::uvcpp_db_client& client,
                       const sql_dialect& dialect, std::string& err) const;
  bool get(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
           const std::string& key, std::string& json, bool& found,
           std::string& err) const;
  bool put(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
           const std::string& key, const std::string& json,
           std::uint64_t now_ms, std::string& err) const;
  bool revision(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                std::int64_t& out, std::string& err) const;
  bool bump_revision(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                     std::int64_t& out, std::string& err) const;
};

class ban_repository {
 public:
  bool add(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
           const ban_record& ban, std::string& err) const;
  bool remove(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
              const std::string& id, bool& found) const;
  bool list(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
            std::size_t offset, std::size_t limit,
            std::vector<ban_record>& out, std::size_t& total,
            std::string& err) const;
  bool match(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
             const ip_addr& address, std::uint64_t now_ms, ban_record& out,
             bool& found, std::string& err) const;
  bool prune_expired(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                     std::uint64_t now_ms) const;
};

class audit_repository {
 public:
  bool append(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
              const audit_record& record, std::string& err) const;
  bool list(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
            std::size_t offset, std::size_t limit,
            std::vector<audit_record>& out, std::size_t& total,
            std::string& err) const;
  bool prune_older_than(uvcpp::uvcpp_db_client& client,
                        const sql_dialect& dialect, std::uint64_t cutoff_ms) const;
};

}  // namespace molink
