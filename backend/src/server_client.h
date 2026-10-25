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

/// One ICE signaling session from the directory (see /v1/connect*).
struct signal_session_info {
  std::string connect_id;
  std::string from_id;
  std::string from_name;
  std::string offer;
  std::vector<std::string> from_candidates;
  std::string answer;
  std::vector<std::string> to_candidates;
  bool answered = false;
  bool closed = false;
  bool expired = false;
  std::uint64_t expires_ms = 0;
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

  // ---- ICE signaling (P2P) ------------------------------------------------
  // These are synchronous HTTP calls; the P2P worker thread calls them. They
  // use the same directory URL, device ID/token and CA as register/heartbeat.
  bool signal_create(const std::string& target, const std::string& offer,
                     const std::vector<std::string>& candidates,
                     std::string& connect_id, std::string& error);
  bool signal_pending(std::vector<signal_session_info>& out, std::string& error);
  bool signal_answer(const std::string& connect_id, const std::string& answer,
                     const std::vector<std::string>& candidates,
                     std::string& error);
  bool signal_status(const std::string& connect_id,
                     signal_session_info& out, std::string& error);
  bool signal_candidates(const std::string& connect_id,
                         const std::vector<std::string>& candidates,
                         std::string& error);
  bool signal_close(const std::string& connect_id, std::string& error);

  /// GET /v1/devices through this device's directory credentials. The body is
  /// returned verbatim and `http_status` is the directory's status; the browser
  /// cannot do this itself because it would have to trust the directory's
  /// private CA and go cross-origin.
  bool directory_lookup(const std::string& query, std::string& body,
                        int& http_status, std::string& error);

 private:
  struct signaling_credentials {
    std::string url;
    std::string device_id;
    std::string token;
  };
  signaling_credentials signaling_credentials_locked() const;
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
  /// Serialises server.json writes: the UI thread rotates a domain while the
  /// worker may save a heartbeat, and both use the same temporary file.
  std::mutex credentials_mutex_;
  /// libuvcpp's synchronous HTTP client is not safe to use from two threads at
  /// once (global TLS/loop state). The heartbeat thread and the P2P signaling
  /// worker share this directory client, so every request is serialised.
  std::mutex http_mutex_;
  server_status status_;
  std::string token_;
  std::string active_url_;
  uint64_t last_attempt_ms_ = 0;
  int failures_ = 0;
};

}  // namespace molink
