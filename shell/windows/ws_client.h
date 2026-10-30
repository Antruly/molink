// Minimal WebSocket client for the native shell: blocking Winsock, one reader
// thread, text frames only. Enough to talk to the local core's /ws/session,
// /ws/direct and /ws/connect endpoints on Windows 7 and later.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace molink::net {

class ws_client {
 public:
  ws_client() = default;
  ~ws_client();
  ws_client(const ws_client&) = delete;
  ws_client& operator=(const ws_client&) = delete;

  /// Opens the TCP connection and performs the HTTP Upgrade handshake.
  bool connect(const std::string& host, int port, const std::string& path,
               std::string* error);
  bool send_text(const std::string& text);
  void close();
  bool is_open() const { return running_.load(); }

  /// Called on the reader thread for every complete text frame.
  std::function<void(const std::string&)> on_text;
  /// Called once when the connection ends (remote close, error, close()).
  std::function<void()> on_close;

 private:
  bool handshake(const std::string& host, int port, const std::string& path,
                 std::string* error);
  bool write_frame(uint8_t opcode, const uint8_t* data, size_t len);
  void reader_loop();
  bool read_exact(void* buffer, size_t len);
  void stop_reader();

  uintptr_t socket_ = ~static_cast<uintptr_t>(0);
  std::thread reader_;
  std::atomic<bool> running_{false};
};

}  // namespace molink::net
