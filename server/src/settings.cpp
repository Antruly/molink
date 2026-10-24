#include "settings.h"

#include <nlohmann/json.hpp>

namespace molink {

std::string default_server_settings_json() {
  nlohmann::json j;
  j["name"] = "moLink Server";
  j["public_url"] = "";
  j["registration_token"] = "";
  j["offline_after_seconds"] = 90;
  j["max_devices"] = 10000;
  return j.dump();
}

std::string default_security_settings_json() {
  nlohmann::json j;
  j["ip_policy"] = {
      {"allow_private", true},
      {"allow_mainland_china", true},
      {"allow_overseas", false},
      {"custom_allow", nlohmann::json::array()},
      {"custom_deny", nlohmann::json::array()},
      {"apply_to_admin", true},
  };
  j["trusted_proxies"] = nlohmann::json::array();
  j["proxy_header"] = "X-Forwarded-For";
  j["limits"] = {
      {"device_ip_requests_per_minute", 600},
      {"device_ip_burst", 120},
      {"register_per_ip_per_hour", 10},
      {"register_burst", 3},
      {"devices_per_ip", 50},
      {"heartbeat_per_device_per_minute", 6},
      {"heartbeat_burst", 3},
      {"heartbeat_per_ip_per_minute", 240},
      {"lookup_per_ip_per_minute", 60},
      {"lookup_burst", 30},
      {"unregister_per_ip_per_minute", 10},
      {"unregister_burst", 5},
      {"admin_api_per_session_per_minute", 300},
      {"admin_api_burst", 60},
      {"admin_login_per_ip_per_5min", 10},
      {"setup_attempts_per_ip_per_10min", 5},
  };
  j["login"] = {
      {"failure_limit", 5},
      {"failure_window_seconds", 900},
      {"ban_ladder_seconds", {300, 1800, 21600, 86400}},
      {"strike_reset_seconds", 86400},
      {"throttle_loopback", false},
  };
  j["session"] = {
      {"idle_seconds", 1800},
      {"absolute_seconds", 43200},
      {"secure_cookie", "auto"},
      {"trust_forwarded_proto", true},
  };
  j["password"] = {{"min_length", 10}, {"max_length", 256}};
  return j.dump();
}

std::string default_relay_settings_json() {
  nlohmann::json j;
  j["enabled"] = false;
  j["bandwidth_limit_kbps"] = 0;
  j["per_device_limit_kbps"] = 0;
  j["supported"] = false;
  return j.dump();
}

}  // namespace molink
