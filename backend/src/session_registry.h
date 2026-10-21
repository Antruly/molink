#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace uvcpp {
class uvcpp_ws_connection;
}

namespace molink {

struct session_info {
  uint32_t id = 0;
  std::string public_id;      ///< e.g. "A3F-9K2-X7Q", the molink://session id
  std::string peer_ip;
  unsigned int peer_port = 0;
  bool authenticated = false;
  uint64_t connected_ms = 0;
  uint64_t last_seen_ms = 0;
  uint64_t last_ping_ms = 0;
  uint64_t frames_sent = 0;
  uvcpp::uvcpp_ws_connection* conn = nullptr;
};

/// Tracks every live WebSocket session. The registry owns the records, not the
/// connections: libuvcpp owns those and recycles them on close. Callers hold a
/// shared_ptr so a record cannot disappear between a lookup and a send.
class session_registry {
 public:
  std::shared_ptr<session_info> add(uvcpp::uvcpp_ws_connection* conn,
                                    const std::string& peer_ip,
                                    unsigned int peer_port);
  void remove(uint32_t id);
  std::shared_ptr<session_info> find(uint32_t id);
  std::shared_ptr<session_info> find_by_public_id(const std::string& public_id);
  std::vector<session_info> snapshot() const;
  std::size_t size() const;
  void touch(session_info& s);

 private:
  mutable std::mutex mu_;
  std::vector<std::shared_ptr<session_info>> sessions_;
  uint32_t next_id_ = 1;
};

}  // namespace molink
