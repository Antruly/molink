#include "settings_cache.h"

#include <nlohmann/json.hpp>

#include <algorithm>

#include "settings.h"
#include "util.h"

namespace molink {
namespace {

using nlohmann::json;

int json_int(const json& object, const char* key, int fallback) {
  if (!object.contains(key) || !object[key].is_number_integer()) return fallback;
  return object[key].get<int>();
}

bool json_bool(const json& object, const char* key, bool fallback) {
  if (!object.contains(key) || !object[key].is_boolean()) return fallback;
  return object[key].get<bool>();
}

std::string json_string(const json& object, const char* key,
                        const std::string& fallback) {
  if (!object.contains(key) || !object[key].is_string()) return fallback;
  return object[key].get<std::string>();
}

bool parse_cidr_list(const json& object, const char* key,
                     std::vector<cidr_block>& out, std::string& error) {
  out.clear();
  if (!object.contains(key)) return true;
  if (!object[key].is_array()) {
    error = std::string(key) + " must be an array of CIDRs";
    return false;
  }
  for (const json& item : object[key]) {
    if (!item.is_string()) {
      error = std::string(key) + " must contain CIDR strings";
      return false;
    }
    cidr_block block;
    std::string detail;
    if (!parse_cidr(item.get<std::string>(), block, &detail)) {
      error = std::string(key) + ": " + detail;
      return false;
    }
    out.push_back(block);
  }
  return true;
}

bool parse_policy(const json& object, ip_policy& out, std::string& error) {
  out = ip_policy{};
  if (!object.contains("ip_policy")) return true;
  const json& policy = object["ip_policy"];
  if (!policy.is_object()) {
    error = "ip_policy must be an object";
    return false;
  }
  out.allow_private = json_bool(policy, "allow_private", out.allow_private);
  out.allow_mainland_china =
      json_bool(policy, "allow_mainland_china", out.allow_mainland_china);
  out.allow_overseas =
      json_bool(policy, "allow_overseas", out.allow_overseas);
  return parse_cidr_list(policy, "custom_allow", out.custom_allow, error) &&
         parse_cidr_list(policy, "custom_deny", out.custom_deny, error);
}

}  // namespace

bool settings_cache::parse_server(const std::string& text, server_settings& out,
                                  std::string& error) {
  json document;
  try {
    document = json::parse(text);
  } catch (const std::exception& e) {
    error = std::string("server settings are not valid JSON: ") + e.what();
    return false;
  }
  if (!document.is_object()) {
    error = "server settings must be a JSON object";
    return false;
  }
  server_settings parsed;
  parsed.name = json_string(document, "name", parsed.name);
  parsed.public_url = json_string(document, "public_url", parsed.public_url);
  parsed.registration_token =
      json_string(document, "registration_token", parsed.registration_token);
  parsed.offline_after_seconds =
      json_int(document, "offline_after_seconds", parsed.offline_after_seconds);
  parsed.max_devices = json_int(document, "max_devices", parsed.max_devices);
  if (parsed.offline_after_seconds < 1 ||
      parsed.offline_after_seconds > 86400) {
    error = "offline_after_seconds must be 1..86400";
    return false;
  }
  if (parsed.max_devices < 1 || parsed.max_devices > 1000000) {
    error = "max_devices must be 1..1000000";
    return false;
  }
  out = std::move(parsed);
  return true;
}

bool settings_cache::parse_security(const std::string& text,
                                    security_settings& out,
                                    std::string& error) {
  json document;
  try {
    document = json::parse(text);
  } catch (const std::exception& e) {
    error = std::string("security settings are not valid JSON: ") + e.what();
    return false;
  }
  if (!document.is_object()) {
    error = "security settings must be a JSON object";
    return false;
  }
  security_settings parsed;
  if (!parse_policy(document, parsed.policy, error)) return false;
  if (!parse_cidr_list(document, "trusted_proxies", parsed.trusted_proxies,
                       error)) {
    return false;
  }

  if (document.contains("limits") && document["limits"].is_object()) {
    const json& limits = document["limits"];
    limit_settings& target = parsed.limits;
    target.device_ip_per_minute =
        json_int(limits, "device_ip_requests_per_minute",
                 target.device_ip_per_minute);
    target.device_ip_burst =
        json_int(limits, "device_ip_burst", target.device_ip_burst);
    target.register_per_hour =
        json_int(limits, "register_per_ip_per_hour", target.register_per_hour);
    target.register_burst =
        json_int(limits, "register_burst", target.register_burst);
    target.devices_per_ip =
        json_int(limits, "devices_per_ip", target.devices_per_ip);
    target.heartbeat_per_device_per_minute =
        json_int(limits, "heartbeat_per_device_per_minute",
                 target.heartbeat_per_device_per_minute);
    target.heartbeat_burst =
        json_int(limits, "heartbeat_burst", target.heartbeat_burst);
    target.heartbeat_per_ip_per_minute =
        json_int(limits, "heartbeat_per_ip_per_minute",
                 target.heartbeat_per_ip_per_minute);
    target.lookup_per_minute =
        json_int(limits, "lookup_per_ip_per_minute", target.lookup_per_minute);
    target.lookup_burst =
        json_int(limits, "lookup_burst", target.lookup_burst);
    target.unregister_per_minute =
        json_int(limits, "unregister_per_ip_per_minute",
                 target.unregister_per_minute);
    target.unregister_burst =
        json_int(limits, "unregister_burst", target.unregister_burst);
    target.admin_api_per_minute =
        json_int(limits, "admin_api_per_session_per_minute",
                 target.admin_api_per_minute);
    target.admin_api_burst =
        json_int(limits, "admin_api_burst", target.admin_api_burst);
    target.admin_login_per_5min =
        json_int(limits, "admin_login_per_ip_per_5min",
                 target.admin_login_per_5min);
    target.setup_per_10min =
        json_int(limits, "setup_attempts_per_ip_per_10min",
                 target.setup_per_10min);
  }

  if (document.contains("login") && document["login"].is_object()) {
    const json& login = document["login"];
    login_settings& target = parsed.login;
    target.failure_limit =
        json_int(login, "failure_limit", target.failure_limit);
    target.failure_window_seconds =
        json_int(login, "failure_window_seconds",
                 target.failure_window_seconds);
    target.strike_reset_seconds =
        json_int(login, "strike_reset_seconds", target.strike_reset_seconds);
    target.throttle_loopback =
        json_bool(login, "throttle_loopback", target.throttle_loopback);
    if (login.contains("ban_ladder_seconds")) {
      if (!login["ban_ladder_seconds"].is_array()) {
        error = "ban_ladder_seconds must be an array";
        return false;
      }
      std::vector<int> ladder;
      for (const json& item : login["ban_ladder_seconds"]) {
        if (!item.is_number_integer() || item.get<int>() <= 0) {
          error = "ban_ladder_seconds must contain positive integers";
          return false;
        }
        ladder.push_back(item.get<int>());
      }
      if (!ladder.empty()) target.ban_ladder_seconds = std::move(ladder);
    }
  }

  if (document.contains("session") && document["session"].is_object()) {
    const json& session = document["session"];
    session_settings& target = parsed.session;
    target.idle_seconds =
        json_int(session, "idle_seconds", target.idle_seconds);
    target.absolute_seconds =
        json_int(session, "absolute_seconds", target.absolute_seconds);
    target.secure_cookie =
        json_string(session, "secure_cookie", target.secure_cookie);
    target.trust_forwarded_proto =
        json_bool(session, "trust_forwarded_proto",
                  target.trust_forwarded_proto);
  }

  if (document.contains("password") && document["password"].is_object()) {
    parsed.password_min_length =
        json_int(document["password"], "min_length",
                 parsed.password_min_length);
  }

  const limit_settings& limits = parsed.limits;
  if (limits.device_ip_per_minute < 1 || limits.device_ip_burst < 1 ||
      limits.register_per_hour < 1 || limits.register_burst < 1 ||
      limits.devices_per_ip < 1 || limits.heartbeat_per_device_per_minute < 1 ||
      limits.heartbeat_burst < 1 || limits.heartbeat_per_ip_per_minute < 1 ||
      limits.lookup_per_minute < 1 || limits.lookup_burst < 1 ||
      limits.unregister_per_minute < 1 || limits.unregister_burst < 1 ||
      limits.admin_api_per_minute < 1 || limits.admin_api_burst < 1 ||
      limits.admin_login_per_5min < 1 || limits.setup_per_10min < 1) {
    error = "rate limits must be positive";
    return false;
  }
  if (parsed.login.failure_limit < 1 || parsed.login.failure_window_seconds < 1 ||
      parsed.login.strike_reset_seconds < 1) {
    error = "login thresholds must be positive";
    return false;
  }
  if (parsed.session.idle_seconds < 60 ||
      parsed.session.absolute_seconds < parsed.session.idle_seconds) {
    error = "session timeouts are invalid";
    return false;
  }
  if (parsed.session.secure_cookie != "auto" &&
      parsed.session.secure_cookie != "always" &&
      parsed.session.secure_cookie != "never") {
    error = "secure_cookie must be auto, always or never";
    return false;
  }
  out = std::move(parsed);
  return true;
}

bool settings_cache::load(uvcpp::uvcpp_db_client& client,
                          const sql_dialect& dialect,
                          settings_repository& repository,
                          std::string& error) {
  if (!repository.ensure_defaults(client, dialect, error)) return false;
  std::string server_json;
  std::string security_json;
  bool found = false;
  if (!repository.get(client, dialect, "server", server_json, found, error)) {
    return false;
  }
  if (!found) server_json = default_server_settings_json();
  if (!repository.get(client, dialect, "security", security_json, found,
                      error)) {
    return false;
  }
  if (!found) security_json = default_security_settings_json();

  server_settings server;
  security_settings security;
  if (!parse_server(server_json, server, error)) return false;
  if (!parse_security(security_json, security, error)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  server_ = std::move(server);
  security_ = std::move(security);
  return true;
}

server_settings settings_cache::server() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return server_;
}

security_settings settings_cache::security() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return security_;
}

bool settings_cache::save_server(uvcpp::uvcpp_db_client& client,
                                 const sql_dialect& dialect,
                                 settings_repository& repository,
                                 const std::string& json, std::string& error) {
  server_settings parsed;
  if (!parse_server(json, parsed, error)) return false;
  if (!repository.put(client, dialect, "server", json, epoch_ms(), error)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  server_ = std::move(parsed);
  return true;
}

bool settings_cache::save_security(uvcpp::uvcpp_db_client& client,
                                   const sql_dialect& dialect,
                                   settings_repository& repository,
                                   const std::string& json,
                                   std::string& error) {
  security_settings parsed;
  if (!parse_security(json, parsed, error)) return false;
  if (!repository.put(client, dialect, "security", json, epoch_ms(), error)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  security_ = std::move(parsed);
  return true;
}

}  // namespace molink
