#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molink {

struct device_record {
  std::string id;
  std::string token_hash;
  std::string name;
  std::string address;
  std::string peer_ip;
  int port = 0;
  std::string version;
  std::uint64_t created_ms = 0;
  std::uint64_t last_seen_ms = 0;
};

struct device_query {
  std::size_t page = 1;
  std::size_t page_size = 50;
  std::string search;
  /// -1 = all, 0 = offline, 1 = online.
  int online = -1;
  std::string sort = "last_seen";
};

struct device_page {
  std::vector<device_record> items;
  std::size_t total = 0;
};

struct admin_user {
  std::string id;
  std::string username;
  std::string username_norm;
  std::string display_name;
  std::string role = "viewer";
  std::string password_algo = "pbkdf2-sha256";
  int password_iterations = 600000;
  std::string password_salt;
  std::string password_hash;
  std::uint64_t created_ms = 0;
  std::uint64_t updated_ms = 0;
  std::uint64_t last_login_ms = 0;
  bool disabled = false;
  bool must_change_password = false;
};

struct session_record {
  std::string id;
  std::string token_sha256;
  std::string user_id;
  std::uint64_t created_ms = 0;
  std::uint64_t last_seen_ms = 0;
  std::uint64_t expires_ms = 0;
  std::string ip;
  std::string user_agent;
};

struct ban_record {
  std::string id;
  std::string target;
  std::string target_kind;
  std::string reason;
  std::string source;
  std::uint64_t created_ms = 0;
  std::uint64_t expires_ms = 0;
  std::string created_by;
  std::string note;
};

struct audit_record {
  std::int64_t id = 0;
  std::uint64_t ts_ms = 0;
  std::string event;
  std::string result;
  std::string actor_id;
  std::string actor_name;
  std::string role;
  std::string ip;
  std::string target;
  std::string details_json;
};

}  // namespace molink
