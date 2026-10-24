#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <db/uvcpp_db.h>

#include "db/repositories.h"
#include "security/ip.h"

namespace molink {

struct server_settings {
  std::string name = "moLink Server";
  std::string public_url;
  std::string registration_token;
  int offline_after_seconds = 90;
  int max_devices = 10000;
};

struct limit_settings {
  int device_ip_per_minute = 600;
  int device_ip_burst = 120;
  int register_per_hour = 10;
  int register_burst = 3;
  int devices_per_ip = 50;
  int heartbeat_per_device_per_minute = 6;
  int heartbeat_burst = 3;
  int heartbeat_per_ip_per_minute = 240;
  int lookup_per_minute = 60;
  int lookup_burst = 30;
  int unregister_per_minute = 10;
  int unregister_burst = 5;
  int admin_api_per_minute = 300;
  int admin_api_burst = 60;
  int admin_login_per_5min = 10;
  int setup_per_10min = 5;
};

struct login_settings {
  int failure_limit = 5;
  int failure_window_seconds = 900;
  std::vector<int> ban_ladder_seconds{300, 1800, 21600, 86400};
  int strike_reset_seconds = 86400;
  bool throttle_loopback = false;
};

struct session_settings {
  int idle_seconds = 1800;
  int absolute_seconds = 43200;
  std::string secure_cookie = "auto";
  bool trust_forwarded_proto = true;
};

struct security_settings {
  ip_policy policy;
  std::vector<cidr_block> trusted_proxies;
  limit_settings limits;
  login_settings login;
  session_settings session;
  int password_min_length = 10;
};

/// Thread-safe view of the two JSON settings rows. The admin API writes
/// through this object so live requests see a consistent snapshot.
class settings_cache {
 public:
  bool load(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
            settings_repository& repository, std::string& error);

  server_settings server() const;
  security_settings security() const;

  /// Parse, validate, persist and publish a new JSON document.
  bool save_server(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                   settings_repository& repository, const std::string& json,
                   std::string& error);
  bool save_security(uvcpp::uvcpp_db_client& client, const sql_dialect& dialect,
                     settings_repository& repository, const std::string& json,
                     std::string& error);

  static bool parse_server(const std::string& json, server_settings& out,
                           std::string& error);
  static bool parse_security(const std::string& json, security_settings& out,
                             std::string& error);

 private:
  mutable std::mutex mutex_;
  server_settings server_;
  security_settings security_;
};

}  // namespace molink
