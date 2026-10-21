#include "session_registry.h"

#include <algorithm>

#include <web/uvcpp_ws_connection.h>

#include "util.h"

namespace molink {

std::shared_ptr<session_info> session_registry::add(
    uvcpp::uvcpp_ws_connection* conn, const std::string& peer_ip,
    unsigned int peer_port) {
  std::lock_guard<std::mutex> lock(mu_);
  auto s = std::make_shared<session_info>();
  s->id = next_id_++;
  if (next_id_ == 0) next_id_ = 1;
  s->public_id = make_session_public_id();
  s->peer_ip = peer_ip;
  s->peer_port = peer_port;
  s->connected_ms = now_ms();
  s->last_seen_ms = s->connected_ms;
  s->last_ping_ms = s->connected_ms;
  s->conn = conn;
  sessions_.push_back(s);
  return s;
}

void session_registry::remove(uint32_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  sessions_.erase(
      std::remove_if(sessions_.begin(), sessions_.end(),
                     [id](const std::shared_ptr<session_info>& s) {
                       return s->id == id;
                     }),
      sessions_.end());
}

std::shared_ptr<session_info> session_registry::find(uint32_t id) {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& s : sessions_) {
    if (s->id == id) return s;
  }
  return nullptr;
}

std::shared_ptr<session_info> session_registry::find_by_public_id(
    const std::string& public_id) {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& s : sessions_) {
    if (s->public_id == public_id) return s;
  }
  return nullptr;
}

std::vector<session_info> session_registry::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<session_info> out;
  out.reserve(sessions_.size());
  for (const auto& s : sessions_) out.push_back(*s);
  return out;
}

std::size_t session_registry::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sessions_.size();
}

bool session_registry::has_authenticated() const {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& s : sessions_) {
    if (s->authenticated) return true;
  }
  return false;
}

void session_registry::touch(session_info& s) { s.last_seen_ms = now_ms(); }

}  // namespace molink
