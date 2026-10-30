#include "ws_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

namespace molink::net {
namespace {

constexpr uintptr_t kInvalid = ~static_cast<uintptr_t>(0);

std::string base64(const uint8_t* data, size_t len) {
  static const char* kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t a = data[i];
    const uint32_t b = i + 1 < len ? data[i + 1] : 0;
    const uint32_t c = i + 2 < len ? data[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(kTable[(triple >> 18) & 0x3f]);
    out.push_back(kTable[(triple >> 12) & 0x3f]);
    out.push_back(i + 1 < len ? kTable[(triple >> 6) & 0x3f] : '=');
    out.push_back(i + 2 < len ? kTable[triple & 0x3f] : '=');
  }
  return out;
}

bool init_winsock() {
  static bool ready = false;
  static WSADATA data{};
  if (!ready) {
    ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }
  return ready;
}

}  // namespace

ws_client::~ws_client() { close(); }

bool ws_client::connect(const std::string& host, int port,
                        const std::string& path, std::string* error) {
  close();
  if (!init_winsock()) {
    if (error != nullptr) *error = "WSAStartup failed";
    return false;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) {
    if (error != nullptr) *error = "cannot resolve " + host;
    return false;
  }
  SOCKET sock = INVALID_SOCKET;
  for (addrinfo* it = result; it != nullptr && sock == INVALID_SOCKET;
       it = it->ai_next) {
    sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (sock == INVALID_SOCKET) continue;
    if (::connect(sock, it->ai_addr, static_cast<int>(it->ai_addrlen)) != 0) {
      closesocket(sock);
      sock = INVALID_SOCKET;
    }
  }
  freeaddrinfo(result);
  if (sock == INVALID_SOCKET) {
    if (error != nullptr) *error = "cannot connect to " + host;
    return false;
  }
  DWORD timeout = 5000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  socket_ = static_cast<uintptr_t>(sock);
  if (!handshake(host, port, path, error)) {
    close();
    return false;
  }
  running_.store(true);
  reader_ = std::thread([this] { reader_loop(); });
  return true;
}

bool ws_client::handshake(const std::string& host, int port,
                          const std::string& path, std::string* error) {
  uint8_t key_bytes[16];
  for (int i = 0; i < 16; ++i) {
    key_bytes[i] = static_cast<uint8_t>(rand() & 0xff);
  }
  const std::string key = base64(key_bytes, sizeof(key_bytes));
  std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                        std::to_string(port) +
                        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: " +
                        key +
                        "\r\nSec-WebSocket-Version: 13\r\n\r\n";
  const SOCKET sock = static_cast<SOCKET>(socket_);
  if (::send(sock, request.data(), static_cast<int>(request.size()), 0) !=
      static_cast<int>(request.size())) {
    if (error != nullptr) *error = "handshake send failed";
    return false;
  }
  std::string response;
  char buffer[1024];
  while (response.find("\r\n\r\n") == std::string::npos) {
    const int got = ::recv(sock, buffer, sizeof(buffer), 0);
    if (got <= 0) {
      if (error != nullptr) *error = "handshake read failed";
      return false;
    }
    response.append(buffer, static_cast<size_t>(got));
    if (response.size() > 16 * 1024) break;
  }
  if (response.rfind("HTTP/1.1 101", 0) != 0 &&
      response.rfind("HTTP/1.1 200", 0) != 0) {
    if (error != nullptr) *error = "unexpected handshake: " + response.substr(0, 40);
    return false;
  }
  return true;
}

bool ws_client::send_text(const std::string& text) {
  return write_frame(0x1, reinterpret_cast<const uint8_t*>(text.data()),
                     text.size());
}

bool ws_client::write_frame(uint8_t opcode, const uint8_t* data, size_t len) {
  if (!running_.load()) return false;
  std::vector<uint8_t> frame;
  frame.push_back(static_cast<uint8_t>(0x80 | opcode));
  if (len < 126) {
    frame.push_back(static_cast<uint8_t>(0x80 | len));
  } else if (len < 65536) {
    frame.push_back(0x80 | 126);
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    frame.push_back(static_cast<uint8_t>(len & 0xff));
  } else {
    frame.push_back(0x80 | 127);
    for (int i = 7; i >= 0; --i) {
      frame.push_back(static_cast<uint8_t>((static_cast<uint64_t>(len) >> (8 * i)) & 0xff));
    }
  }
  uint8_t mask[4] = {static_cast<uint8_t>(rand() & 0xff),
                     static_cast<uint8_t>(rand() & 0xff),
                     static_cast<uint8_t>(rand() & 0xff),
                     static_cast<uint8_t>(rand() & 0xff)};
  frame.insert(frame.end(), mask, mask + 4);
  const size_t header = frame.size();
  frame.resize(header + len);
  for (size_t i = 0; i < len; ++i) frame[header + i] = data[i] ^ mask[i % 4];
  const SOCKET sock = static_cast<SOCKET>(socket_);
  size_t sent = 0;
  while (sent < frame.size()) {
    const int n = ::send(sock, reinterpret_cast<const char*>(frame.data() + sent),
                         static_cast<int>(frame.size() - sent), 0);
    if (n <= 0) {
      running_.store(false);
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool ws_client::read_exact(void* buffer, size_t len) {
  const SOCKET sock = static_cast<SOCKET>(socket_);
  size_t got = 0;
  while (got < len) {
    const int n = ::recv(sock, static_cast<char*>(buffer) + got,
                         static_cast<int>(len - got), 0);
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
  }
  return true;
}

void ws_client::reader_loop() {
  std::string assembled;
  while (running_.load()) {
    uint8_t head[2];
    if (!read_exact(head, 2)) break;
    const bool fin = (head[0] & 0x80) != 0;
    const uint8_t opcode = head[0] & 0x0f;
    const bool masked = (head[1] & 0x80) != 0;
    uint64_t len = head[1] & 0x7f;
    if (len == 126) {
      uint8_t ext[2];
      if (!read_exact(ext, 2)) break;
      len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
      uint8_t ext[8];
      if (!read_exact(ext, 8)) break;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
    }
    if (len > 64ull * 1024 * 1024) break;
    uint8_t mask[4] = {};
    if (masked && !read_exact(mask, 4)) break;
    std::vector<uint8_t> payload(static_cast<size_t>(len));
    if (len > 0 && !read_exact(payload.data(), static_cast<size_t>(len))) break;
    if (masked) {
      for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
    }
    if (opcode == 0x8) break;              // close
    if (opcode == 0x9) {                   // ping -> pong
      write_frame(0xA, payload.data(), payload.size());
      continue;
    }
    if (opcode == 0xA) continue;           // pong
    if (!fin) {
      assembled.append(reinterpret_cast<const char*>(payload.data()),
                       payload.size());
      continue;
    }
    if (opcode == 0x0) {
      assembled.append(reinterpret_cast<const char*>(payload.data()),
                       payload.size());
      if (on_text) on_text(assembled);
      assembled.clear();
    } else if (opcode == 0x1) {
      if (!assembled.empty()) {
        assembled.append(reinterpret_cast<const char*>(payload.data()),
                         payload.size());
        if (on_text) on_text(assembled);
        assembled.clear();
      } else if (on_text) {
        on_text(std::string(reinterpret_cast<const char*>(payload.data()),
                            payload.size()));
      }
    }
  }
  running_.store(false);
  if (on_close) on_close();
}

void ws_client::stop_reader() {
  if (reader_.joinable()) {
    if (reader_.get_id() != std::this_thread::get_id()) {
      reader_.join();
    } else {
      reader_.detach();
    }
  }
}

void ws_client::close() {
  running_.store(false);
  if (socket_ != kInvalid) {
    shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
    closesocket(static_cast<SOCKET>(socket_));
    socket_ = kInvalid;
  }
  stop_reader();
}

}  // namespace molink::net
