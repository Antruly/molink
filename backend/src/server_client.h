#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace molink {

class auth_state;

/// Status shown in the UI and returned by /api/state.
struct server_status {
  bool enabled = false;     ///< A server URL is configured.
  bool registered = false;  ///< This core has an ID from that server.
  bool online = false;      ///< A heartbeat succeeded recently.
  std::string url;
  std::string device_id;
  std::string display_id;
  std::string last_error;
  uint64_t last_heartbeat_ms = 0;
};

/// Runs in its own thread: registers this core with the configured directory,
/// sends a heartbeat every 30 seconds, and keeps the assigned ID in
/// server.json so a restart reuses it. HTTP only for now; put TLS in front of
/// the server until the client gains an SSL path.
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

 private:
  void run();
  bool try_register(const std::string& url, const std::string& invite);
  bool try_heartbeat();
  bool post(const std::string& base_url, const std::string& path,
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
