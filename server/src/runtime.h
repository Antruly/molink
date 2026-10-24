#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <webapp/uvcpp_web_app.h>

#include "auth/auth_service.h"
#include "config.h"
#include "db/database.h"
#include "db/db_executor.h"
#include "db/repositories.h"
#include "security/ban_cache.h"
#include "security/rate_limiter.h"
#include "settings_cache.h"

namespace molink {

class server_runtime {
 public:
  explicit server_runtime(server_config config);
  ~server_runtime();

  server_runtime(const server_runtime&) = delete;
  server_runtime& operator=(const server_runtime&) = delete;

  bool initialize(std::string& error);
  bool start(std::string& error);
  void request_stop();
  void tick();
  void shutdown();
  int exit_code() const { return exit_code_; }

  server_config& config() { return config_; }
  database& db() { return db_; }
  db_executor& executor() { return executor_; }
  device_repository& devices() { return devices_; }
  user_repository& users() { return users_; }
  session_repository& sessions() { return sessions_; }
  settings_repository& settings_repository_ref() { return settings_repo_; }
  ban_repository& bans_repository() { return bans_repo_; }
  audit_repository& audit_repository_ref() { return audit_repo_; }
  auth_service& auth() { return auth_; }
  settings_cache& settings() { return settings_; }
  ban_cache& bans() { return ban_cache_; }
  rate_limiter& limiter() { return limiter_; }
  uvcpp::uvcpp_web_app& device_app() { return device_app_; }
  uvcpp::uvcpp_web_app& admin_app() { return admin_app_; }
  std::atomic<bool>& stopping() { return stopping_; }

  bool device_tls() const { return !config_.device.tls_cert.empty(); }
  bool admin_tls() const { return !config_.admin.tls_cert.empty(); }
  std::uint64_t started_ms() const { return started_ms_; }
  const std::string& ui_root() const { return ui_root_; }

  /// Write an audit event through the executor (best effort).
  void audit(const std::string& event, const std::string& result,
             const std::string& actor_id, const std::string& actor_name,
             const std::string& role, const std::string& ip,
             const std::string& target, const std::string& details_json = "");

 private:
  server_config config_;
  database db_;
  db_executor executor_;
  device_repository devices_;
  user_repository users_;
  session_repository sessions_;
  settings_repository settings_repo_;
  ban_repository bans_repo_;
  audit_repository audit_repo_;
  auth_service auth_;
  settings_cache settings_;
  ban_cache ban_cache_;
  rate_limiter limiter_;
  uvcpp::uvcpp_web_app device_app_;
  uvcpp::uvcpp_web_app admin_app_;
  std::atomic<bool> stopping_{false};
  bool started_ = false;
  bool shutdown_done_ = false;
  std::uint64_t started_ms_ = 0;
  std::uint64_t last_prune_ms_ = 0;
  std::uint64_t last_audit_prune_ms_ = 0;
  std::string ui_root_;
  int exit_code_ = 0;
};

}  // namespace molink
