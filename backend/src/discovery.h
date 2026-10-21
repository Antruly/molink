#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "auth_state.h"

namespace molink {

struct discovered_peer {
  std::string device_id;
  std::string name;
  std::string host;
  int port = 0;
  uint64_t last_seen_ms = 0;
};

/// UDP multicast beacon for the local LAN. Off by default; only starts when
/// both allow_remote_access and discovery_enabled are on. Beacons contain the
/// device id, name and web port — never a token.
class discovery_service {
 public:
  discovery_service(std::shared_ptr<auth_state> auth, std::string device_id,
                    int web_port);
  ~discovery_service();

  discovery_service(const discovery_service&) = delete;
  discovery_service& operator=(const discovery_service&) = delete;

  void set_web_port(int port);
  void set_enabled(bool enabled);
  bool running() const { return running_.load(); }
  std::vector<discovered_peer> peers() const;

  static constexpr int kPort = 28423;
  static constexpr const char* kGroup = "239.255.42.99";

 private:
  void run();
  void close_socket();

  std::shared_ptr<auth_state> auth_;
  std::string device_id_;
  std::atomic<int> web_port_{0};

  mutable std::mutex peers_mutex_;
  std::map<std::string, discovered_peer> peers_;

  std::mutex lifecycle_mutex_;
  std::atomic<bool> running_{false};
  std::thread thread_;
#if defined(_WIN32)
  unsigned long long socket_ = ~0ull;
#else
  int socket_ = -1;
#endif
};

}  // namespace molink
