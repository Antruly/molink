#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <webapp/uvcpp_web_ws.h>

#include "capture/capture.h"
#include "input/input.h"
#include "session_registry.h"

namespace uvcpp {
class uvcpp_web_app;
}

namespace molink {

struct ws_options {
  std::string token;
  std::string version = "0.0.0";
  int heartbeat_ms = 15000;
  int hello_timeout_ms = 5000;
  int idle_close_ms = 45000;
  std::shared_ptr<capture_source> capture;
  std::shared_ptr<input_injector> input;
};

/// Handler for WS /ws/session. Authentication is part of the application
/// handshake: the first text message must be {"type":"hello","token":"..."},
/// or the connection may carry a matching ?token= on the upgrade URL.
uvcpp::uvcpp_web_ws_handler make_ws_handler(
    std::shared_ptr<session_registry> registry, ws_options options);

/// Sends keepalive pings and reaps sessions that never authenticated or went
/// silent. Runs its own thread and posts back to the app loop, because libuv
/// handles are loop-affine.
class session_supervisor {
 public:
  session_supervisor(uvcpp::uvcpp_web_app& app,
                     std::shared_ptr<session_registry> registry,
                     ws_options options);
  ~session_supervisor();

  session_supervisor(const session_supervisor&) = delete;
  session_supervisor& operator=(const session_supervisor&) = delete;

  void start();
  void stop();

 private:
  void run();

  uvcpp::uvcpp_web_app& app_;
  std::shared_ptr<session_registry> registry_;
  ws_options options_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace molink
