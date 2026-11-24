#include "session_registry.h"

#include <algorithm>
#include <utility>

#include "util.h"

namespace molink {
namespace {

/// IPv4-mapped IPv6 peers count as the same address as the plain IPv4 form.
std::string normalize_peer(std::string peer) {
  if (peer.compare(0, 7, "::ffff:") == 0) peer = peer.substr(7);
  return peer;
}

}  // namespace

std::shared_ptr<session_info> session_registry::add(
    std::shared_ptr<session_transport> transport, add_status* status) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::string peer =
      normalize_peer(transport ? transport->peer_ip() : std::string());
  std::size_t unauthenticated = 0;
  std::size_t from_peer = 0;
  for (const auto& existing : sessions_) {
    if (existing->authenticated) continue;
    ++unauthenticated;
    if (!peer.empty() && normalize_peer(existing->peer_ip) == peer) {
      ++from_peer;
    }
  }
  if (unauthenticated >= kMaxUnauthenticated) {
    if (status != nullptr) *status = add_status::global_unauthenticated_limit;
    return nullptr;
  }
  if (!peer.empty() && from_peer >= kMaxUnauthenticatedPerIp) {
    if (status != nullptr) *status = add_status::per_ip_unauthenticated_limit;
    return nullptr;
  }
  if (status != nullptr) *status = add_status::ok;
  auto s = std::make_shared<session_info>();
  s->id = next_id_++;
  if (next_id_ == 0) next_id_ = 1;
  s->public_id = make_session_public_id();
  s->peer_ip = transport ? transport->peer_ip() : std::string();
  s->peer_port = transport ? transport->peer_port() : 0;
  s->connected_ms = now_ms();
  s->last_seen_ms = s->connected_ms;
  s->last_ping_ms = s->connected_ms;
  s->transport = std::move(transport);
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

bool session_registry::set_authenticated(
    const std::shared_ptr<session_info>& s) {
  if (!s) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (s->authenticated) return true;
  std::size_t authenticated = 0;
  for (const auto& existing : sessions_) {
    if (existing->authenticated) ++authenticated;
  }
  if (authenticated >= kMaxAuthenticated) return false;
  s->authenticated = true;
  return true;
}

void session_registry::set_awaiting_approval(
    const std::shared_ptr<session_info>& s, bool awaiting) {
  if (!s) return;
  std::lock_guard<std::mutex> lock(mu_);
  s->awaiting_approval = awaiting;
}

bool session_registry::set_capabilities(uint32_t id, uint32_t capabilities) {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& session : sessions_) {
    if (session->id != id) continue;
    session->capabilities = capabilities;
    return true;
  }
  return false;
}

bool session_registry::has_capability(uint32_t id,
                                      capability value) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& session : sessions_) {
    if (session->id == id) {
      return molink::has_capability(session->capabilities, value);
    }
  }
  return false;
}

uint32_t session_registry::capabilities(uint32_t id) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& session : sessions_) {
    if (session->id == id) return session->capabilities;
  }
  return 0;
}

void session_registry::set_clipboard_offer(
    uint32_t id, std::vector<clipboard_offer_entry> files, bool nested_rdp) {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& session : sessions_) {
    if (session->id != id) continue;
    session->clipboard_offer = std::move(files);
    session->clipboard_nested_rdp = nested_rdp;
    return;
  }
}

std::vector<clipboard_offer_entry> session_registry::clipboard_offer(
    uint32_t id) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& session : sessions_) {
    if (session->id == id) return session->clipboard_offer;
  }
  return {};
}

void session_registry::touch(session_info& s) { s.last_seen_ms = now_ms(); }

}  // namespace molink
