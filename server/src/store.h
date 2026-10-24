#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace molink_server {

/// One desktop registered with the directory. `token` authenticates
/// heartbeats and unregistration and never leaves the server.
struct device_record {
  std::string id;
  std::string token;
  std::string name;
  std::string address;   ///< Reported address (may be empty).
  std::string peer_ip;   ///< Source address seen by the server.
  int port = 0;
  std::string version;
  uint64_t created_ms = 0;
  uint64_t last_seen_ms = 0;
};

/// Device directory persisted as one JSON file. A single-threaded HTTP loop
/// owns it; no locking is needed.
class device_store {
 public:
  device_store(std::string path, int offline_after_seconds);

  bool load(std::string& err);
  bool save(std::string& err) const;

  bool register_device(const std::string& name, const std::string& address,
                       const std::string& peer_ip, int port,
                       const std::string& version, device_record& out,
                       std::string& err);
  bool heartbeat(const std::string& id, const std::string& token,
                 const std::string& address, const std::string& peer_ip,
                 int port, const std::string& version, std::string& err);
  bool unregister_device(const std::string& id, const std::string& token);
  bool find(const std::string& id, device_record& out) const;
  bool online(const device_record& record) const;

  std::size_t size() const { return devices_.size(); }
  std::size_t online_count() const;

  /// Writes pending last_seen updates at most once per interval.
  void flush_if_due(uint64_t interval_ms = 5000);

  static std::string normalize_id(const std::string& raw);
  static std::string display_id(const std::string& raw);

 private:
  std::string generate_id() const;
  static std::string generate_token();
  void mark_dirty() const { dirty_ = true; }

  std::string path_;
  int offline_after_seconds_ = 90;
  std::unordered_map<std::string, device_record> devices_;
  mutable bool dirty_ = false;
  mutable uint64_t last_save_ms_ = 0;
};

}  // namespace molink_server
