#include "discovery.h"

#include "identity.h"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "util.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace molink {
namespace {

#if defined(_WIN32)
using socket_handle = SOCKET;
constexpr socket_handle kInvalidSocket = INVALID_SOCKET;
using sock_len = int;
#else
using socket_handle = int;
constexpr socket_handle kInvalidSocket = -1;
using sock_len = socklen_t;
#endif

constexpr uint64_t kBeaconIntervalMs = 2000;
constexpr uint64_t kPeerExpiryMs = 6000;

bool recv_timed_out() {
#if defined(_WIN32)
  const int error = WSAGetLastError();
  return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

#if defined(_WIN32)
void init_winsock() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
  });
}
#else
void init_winsock() {}
#endif

}  // namespace

discovery_service::discovery_service(std::shared_ptr<auth_state> auth,
                                     std::string device_id, int web_port)
    : auth_(std::move(auth)),
      device_id_(std::move(device_id)),
      web_port_(web_port) {
  init_winsock();
}

discovery_service::~discovery_service() { set_enabled(false); }

void discovery_service::set_web_port(int port) { web_port_.store(port); }

void discovery_service::set_enabled(bool enabled) {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (!enabled) {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    close_socket();
    return;
  }
  if (running_.load()) return;
  if (!auth_) return;
  const app_settings settings = auth_->settings();
  if (!settings.allow_remote_access || !settings.discovery_enabled) return;

  const socket_handle fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == kInvalidSocket) return;

  const int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(static_cast<uint16_t>(kPort));
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
    return;
  }

  ip_mreq membership{};
  membership.imr_multiaddr.s_addr = ::inet_addr(kGroup);
  membership.imr_interface.s_addr = htonl(INADDR_ANY);
  ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
               reinterpret_cast<const char*>(&membership),
               sizeof(membership));

  unsigned char ttl = 1;
  ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));
  unsigned char loop = 1;
  ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
               reinterpret_cast<const char*>(&loop), sizeof(loop));

#if defined(_WIN32)
  DWORD timeout_ms = 500;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
  timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 500 * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#endif

  socket_ = fd;
  running_.store(true);
  thread_ = std::thread([this] { run(); });
}

void discovery_service::close_socket() {
  if (socket_ == kInvalidSocket) return;
#if defined(_WIN32)
  ::closesocket(socket_);
#else
  ::close(socket_);
#endif
  socket_ = kInvalidSocket;
}

void discovery_service::run() {
  sockaddr_in group{};
  group.sin_family = AF_INET;
  group.sin_port = htons(static_cast<uint16_t>(kPort));
  group.sin_addr.s_addr = ::inet_addr(kGroup);

  auto last_beacon = std::chrono::steady_clock::now() -
                     std::chrono::milliseconds(kBeaconIntervalMs);
  char buffer[2048];

  while (running_.load()) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_beacon >=
        std::chrono::milliseconds(kBeaconIntervalMs)) {
      last_beacon = now;
      nlohmann::json beacon;
      beacon["magic"] = "MOLINK_DISCOVERY";
      beacon["version"] = 1;
      beacon["device_id"] = device_id_;
      beacon["name"] = auth_ ? auth_->settings().device_name : device_id_;
      beacon["platform"] = device_platform();
      beacon["port"] = web_port_.load();
      beacon["protocol"] = 1;
      const std::string body = beacon.dump();
      if (socket_ != kInvalidSocket) {
        ::sendto(socket_, body.data(), static_cast<int>(body.size()), 0,
                 reinterpret_cast<sockaddr*>(&group), sizeof(group));
      }
    }

    sockaddr_in from{};
    sock_len from_len = sizeof(from);
    const int received =
        socket_ == kInvalidSocket
            ? -1
            : static_cast<int>(::recvfrom(
                  socket_, buffer, static_cast<int>(sizeof(buffer)), 0,
                  reinterpret_cast<sockaddr*>(&from), &from_len));
    if (received > 0) {
      const auto parsed = nlohmann::json::parse(buffer, buffer + received,
                                                nullptr, false);
      if (!parsed.is_discarded() && parsed.is_object() &&
          parsed.value("magic", "") == "MOLINK_DISCOVERY" &&
          parsed.value("version", 0) == 1) {
        discovered_peer peer;
        peer.device_id = parsed.value("device_id", "");
        peer.name = parsed.value("name", "");
        peer.platform = parsed.value("platform", "");
        peer.port = parsed.value("port", 0);
        if (!peer.device_id.empty() && peer.device_id != device_id_ &&
            peer.port > 0 && peer.port <= 65535) {
          char host[INET_ADDRSTRLEN] = {};
          ::inet_ntop(AF_INET, &from.sin_addr, host, sizeof(host));
          peer.host = host;
          peer.last_seen_ms = now_ms();
          std::lock_guard<std::mutex> lock(peers_mutex_);
          peers_[peer.device_id] = std::move(peer);
        }
      }
    } else if (received < 0 && !recv_timed_out()) {
      // Keep running; a transient error should not kill discovery.
    }

    const uint64_t now_ms_value = now_ms();
    std::lock_guard<std::mutex> lock(peers_mutex_);
    for (auto it = peers_.begin(); it != peers_.end();) {
      if (now_ms_value > it->second.last_seen_ms &&
          now_ms_value - it->second.last_seen_ms > kPeerExpiryMs) {
        it = peers_.erase(it);
      } else {
        ++it;
      }
    }
  }

  close_socket();
}

std::vector<discovered_peer> discovery_service::peers() const {
  const uint64_t now = now_ms();
  std::lock_guard<std::mutex> lock(peers_mutex_);
  std::vector<discovered_peer> out;
  out.reserve(peers_.size());
  for (const auto& entry : peers_) {
    const discovered_peer& peer = entry.second;
    if (now > peer.last_seen_ms && now - peer.last_seen_ms > kPeerExpiryMs) {
      continue;
    }
    out.push_back(peer);
  }
  return out;
}

}  // namespace molink
