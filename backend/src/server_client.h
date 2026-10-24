#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace molink {

class auth_state;

/// Status shown in the UI and returned by /api/state.
struct server_status {
  bool enabled = false;      ///< The remote-directory switch is on.
  bool using_default = false;///< No custom URL; the built-in directory is used.
  bool registered = false;   ///< This core has an ID from that server.
  bool online = false;       ///< A heartbeat succeeded recently.
  std::string url;           ///< Effective URL (empty when disabled).
  std::string custom_url;    ///< User-configured URL.
  std::string default_url;   ///< Built-in directory URL.
  std::string device_id;
  std::string display_id;
  std::string last_error;
  /// 9-character group code ("channel") issued by the directory.
  std::string domain;
  /// False when the code was revoked, e.g. its owner rotated it.
  bool domain_valid = false;
  uint64_t last_heartbeat_ms = 0;
};

/// One device on the same domain code, as returned by the directory.
struct domain_peer {
  std::string id;
  std::string display_id;
  std::string name;
  std::string platform;
  std::string avatar;
  std::string host;
  int port = 0;
  bool online = false;
  uint64_t last_seen_ms = 0;
};

/// Runs in its own thread: registers this core with the configured directory,
/// sends a heartbeat every 30 seconds, and keeps the assigned ID in
/// server.json so a restart reuses it. The built-in directory is used when no
/// custom URL is configured and the remote-directory switch is on; HTTPS uses
/// strict certificate and hostname verification.
class server_client {
 public:
  server_client(std::shared_ptr<auth_state> auth,
                std::shared_ptr<std::atomic<int>> bound_port,
                std::string version);
  ~server_client();

  server_client(const server_client&) = delete;
  server_client& operator=(const server_client&) = delete;

  void start();
  void stop();
  server_status status() const;
  /// Wake the thread after settings changed (also detected by polling).
  void reconfigure();

  /// Join a code shared by another device ("join") or ask the directory for a
  /// fresh code owned by this device ("rotate"). A rotate revokes the code the
  /// device owned, so devices that joined it lose their group.
  bool set_domain(const std::string& action, const std::string& code,
                  std::string& err);
  /// Devices that currently share this device's domain code (self excluded).
  bool list_domain_devices(std::vector<domain_peer>& out, std::string& err);

 private:
  void run();
  bool try_register(const std::string& url, const std::string& invite);
  bool try_heartbeat();
  bool request(bool post_method, const std::string& base_url,
               const std::string& path, const std::string& query,
               const std::string& payload, std::string& body,
               int& http_status, std::string& error);
  bool post(const std::string& base_url, const std::string& path,
            const std::string& query, const std::string& payload,
            std::string& body, int& http_status, std::string& error);
  bool get(const std::string& base_url, const std::string& path,
           const std::string& query, std::string& body, int& http_status,
           std::string& error);
  void set_state(bool registered, bool online, const std::string& error);
  bool load_credentials(const std::string& url);
  void save_credentials(const std::string& url);
  void clear_credentials();

  std::shared_ptr<auth_state> auth_;
  std::shared_ptr<std::atomic<int>> bound_port_;
  std::string version_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> reconfigure_{false};
  mutable std::mutex mutex_;
  server_status status_;
  std::string token_;
  std::string active_url_;
  uint64_t last_attempt_ms_ = 0;
  int failures_ = 0;
};

}  // namespace molink
