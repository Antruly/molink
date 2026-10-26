#include "transport/peer_transports.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace molink {
namespace {

#if defined(_WIN32)
using socket_handle = SOCKET;
using socket_length = int;
constexpr socket_handle kInvalidSocket = INVALID_SOCKET;
void close_socket(socket_handle handle) { ::closesocket(handle); }
int last_socket_error() { return WSAGetLastError(); }
#else
using socket_handle = int;
using socket_length = socklen_t;
constexpr socket_handle kInvalidSocket = -1;
void close_socket(socket_handle handle) { ::close(handle); }
int last_socket_error() { return errno; }
#endif

/// One non-blocking connect with a deadline. Returns a connected socket or
/// kInvalidSocket.
socket_handle connect_with_timeout(const addrinfo* addresses, int timeout_ms) {
  for (const addrinfo* entry = addresses; entry != nullptr;
       entry = entry->ai_next) {
    socket_handle handle =
        ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (handle == kInvalidSocket) continue;

#if defined(_WIN32)
    u_long nonblocking = 1;
    ::ioctlsocket(handle, FIONBIO, &nonblocking);
    const int would_block = WSAEWOULDBLOCK;
#else
    const int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags >= 0) ::fcntl(handle, F_SETFL, flags | O_NONBLOCK);
    const int would_block = EINPROGRESS;
#endif

    bool connected = ::connect(handle, entry->ai_addr,
                               static_cast<int>(entry->ai_addrlen)) == 0;
    if (!connected && last_socket_error() == would_block) {
      fd_set writable;
      FD_ZERO(&writable);
      FD_SET(handle, &writable);
      timeval deadline{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
      connected = ::select(static_cast<int>(handle) + 1, nullptr, &writable,
                           nullptr, &deadline) > 0;
      if (connected) {
        int socket_error = 0;
        socket_length length = sizeof(socket_error);
        connected =
            ::getsockopt(handle, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&socket_error), &length) == 0 &&
            socket_error == 0;
      }
    }
    if (!connected) {
      close_socket(handle);
      continue;
    }

    // Back to blocking with a receive/send deadline; the request is small and
    // the peer answers in one or two segments.
#if defined(_WIN32)
    DWORD deadline_ms = static_cast<DWORD>(timeout_ms);
    ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&deadline_ms),
                 sizeof(deadline_ms));
    ::setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&deadline_ms),
                 sizeof(deadline_ms));
    u_long blocking = 0;
    ::ioctlsocket(handle, FIONBIO, &blocking);
#else
    const int flags_now = ::fcntl(handle, F_GETFL, 0);
    if (flags_now >= 0) ::fcntl(handle, F_SETFL, flags_now & ~O_NONBLOCK);
    timeval deadline{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &deadline,
                 sizeof(deadline));
    ::setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &deadline,
                 sizeof(deadline));
#endif
    return handle;
  }
  return kInvalidSocket;
}

bool send_all(socket_handle handle, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const int chunk = ::send(handle, data.data() + sent,
                             static_cast<int>(data.size() - sent), 0);
    if (chunk <= 0) return false;
    sent += static_cast<std::size_t>(chunk);
  }
  return true;
}

}  // namespace

peer_transports probe_peer_transports(const std::string& host, int port,
                                      int timeout_ms) {
  peer_transports result;
  if (host.empty() || port <= 0 || port > 65535) {
    result.error = "invalid peer address";
    return result;
  }

#if defined(_WIN32)
  WSADATA winsock{};
  if (::WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
    result.error = "Winsock is unavailable";
    return result;
  }
#endif

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0 ||
      addresses == nullptr) {
    result.error = "could not resolve the peer address";
    return result;
  }

  socket_handle handle = connect_with_timeout(addresses, timeout_ms);
  ::freeaddrinfo(addresses);
  if (handle == kInvalidSocket) {
    result.error = "could not connect to the peer";
    return result;
  }

  const std::string request = "GET /api/transports HTTP/1.0\r\nHost: " + host +
                              ":" + service +
                              "\r\nConnection: close\r\n\r\n";
  if (!send_all(handle, request)) {
    close_socket(handle);
    result.error = "could not send the transport probe";
    return result;
  }

  std::string response;
  char buffer[4096];
  for (;;) {
    const int received = ::recv(handle, buffer, sizeof(buffer), 0);
    if (received <= 0) break;
    response.append(buffer, static_cast<std::size_t>(received));
    if (response.size() > 4u * 1024u * 1024u) break;
  }
  close_socket(handle);

  const std::size_t header_end = response.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    result.error = "the peer sent no complete HTTP response";
    return result;
  }
  const std::size_t status_start = response.find(' ');
  int status = 0;
  if (status_start != std::string::npos) {
    status = std::atoi(response.c_str() + status_start + 1);
  }
  if (status != 200) {
    result.error = "the peer answered HTTP " + std::to_string(status) +
                   " for /api/transports";
    return result;
  }

  nlohmann::json body;
  try {
    body = nlohmann::json::parse(response.substr(header_end + 4));
  } catch (const std::exception&) {
    result.error = "the peer's /api/transports is not JSON";
    return result;
  }
  if (!body.is_object()) {
    result.error = "the peer's /api/transports is not an object";
    return result;
  }
  result.reachable = true;
  result.tls_cert = body.value("tls_cert", std::string());
  result.tls_fingerprint = body.value("tls_fingerprint", std::string());

  bool quic_available = false;
  bool tcp_available = false;
  bool http2_available = false;
  if (body.contains("transports") && body["transports"].is_array()) {
    for (const auto& item : body["transports"]) {
      if (!item.is_object()) continue;
      const std::string name = item.value("name", std::string());
      const bool available = item.value("available", false);
      if (name == "quic") quic_available = available;
      if (name == "tcp") tcp_available = available;
      if (name == "http2") http2_available = available;
    }
  }

  const auto port_of = [&body](const char* name) {
    if (!body.contains("ports") || !body["ports"].is_object()) return 0;
    const auto& ports = body["ports"];
    if (!ports.contains(name) || !ports[name].is_number_integer()) return 0;
    const int value = ports[name].get<int>();
    return value > 0 && value <= 65535 ? value : 0;
  };
  if (quic_available) result.quic_port = port_of("quic");
  if (tcp_available) result.tcp_port = port_of("tcp_data");
  result.http2 = http2_available;
  return result;
}

}  // namespace molink
