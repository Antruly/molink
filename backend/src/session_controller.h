#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "auth_state.h"
#include "capture/capture.h"
#include "clipboard/clipboard.h"
#include "input/input.h"
#include "lock_state.h"
#include "network_settings.h"
#include "session_registry.h"
#include "transport/session_transport.h"

namespace uvcpp {
class uvcpp_web_app;
}

namespace molink {

struct session_options {
  std::shared_ptr<auth_state> auth;
  std::string version = "0.0.0";
  int heartbeat_ms = 15000;
  int hello_timeout_ms = 5000;
  int idle_close_ms = 45000;
  std::shared_ptr<capture_source> capture;
  std::shared_ptr<input_injector> input;
  std::shared_ptr<clipboard_service> clipboard;
  std::shared_ptr<lock_state> lock;
  std::shared_ptr<network_settings> network;
  std::shared_ptr<std::atomic<int>> bound_port;
  bool http2_available = false;
  int tile_size = 0;
  std::string device_id;
  /// Hop a callback onto the app loop (used for clipboard results, which are
  /// produced on the clipboard thread).
  std::function<void(std::function<void()>)> post;
};

/// Transport-independent session state machine: hello, frame pulls, input,
/// clipboard, cursor and the adaptive capture-rate message. A WebSocket
/// adapter feeds it text/binary messages; a future TCP/QUIC adapter can reuse
/// it by implementing session_transport and calling start().
class session_controller final
    : public std::enable_shared_from_this<session_controller> {
 public:
  session_controller(std::shared_ptr<session_registry> registry,
                     std::shared_ptr<session_transport> transport,
                     session_options options);

  /// Registers the session and installs transport handlers. Returns false when
  /// the peer is blocked/banned or its initial token is rejected (the
  /// transport is closed in that case).
  bool start(const std::string* initial_token);

 private:
  void on_text(const std::string& text);
  void on_binary(const uint8_t* data, std::size_t len);
  void on_close(int code, const std::string& reason);
  void send_welcome_message();

  std::shared_ptr<session_registry> registry_;
  std::weak_ptr<session_transport> transport_;
  std::weak_ptr<session_info> session_;
  session_options options_;
};

/// Sends keepalive pings and reaps sessions that never authenticated or went
/// silent. Runs its own thread and posts back to the app loop, because libuv
/// handles are loop-affine.
class session_supervisor {
 public:
  session_supervisor(uvcpp::uvcpp_web_app& app,
                     std::shared_ptr<session_registry> registry,
                     session_options options);
  ~session_supervisor();

  session_supervisor(const session_supervisor&) = delete;
  session_supervisor& operator=(const session_supervisor&) = delete;

  void start();
  void stop();

 private:
  void run();

  uvcpp::uvcpp_web_app& app_;
  std::shared_ptr<session_registry> registry_;
  session_options options_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace molink
