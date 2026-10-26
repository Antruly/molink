#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "transport/relay_session.h"

namespace molink {

/// Session transport over the directory's /v1/relay WebSocket.
///
/// The relay pairs two devices of one signaling session and forwards whole
/// text/binary messages; there is no ICE and no fragmentation. The libuvcpp
/// WebSocket client owns its own loop, so it runs on a small thread: sends are
/// queued from the app loop, and inbound messages are posted back to it.
class relay_transport final : public relay_session {
 public:
  using post_fn = std::function<void(std::function<void()>)>;

  /// `relay_protocol` is false for a plain session WebSocket (`/ws/session`):
  /// the transport is ready as soon as the socket opens and the
  /// relay-ready/relay-closed texts are ordinary messages.
  relay_transport(std::string url, std::string ca_path, post_fn post,
                  bool relay_protocol = true);
  ~relay_transport() override;

  /// The wire protocol to the relay endpoint, not the path: the relay will
  /// later be able to carry QUIC or another protocol without changing how the
  /// session is classified.
  const char* name() const override { return "websocket"; }
  const char* path() const override { return "relay"; }
  std::string peer_ip() const override { return "relay"; }
  unsigned int peer_port() const override { return 0; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;

  /// Called on the app loop once the server has paired both sides.
  void set_ready_handler(std::function<void()> ready);
  void start();

 private:
  void thread_main();
  bool enqueue(bool text, std::string data);
  void deliver_close(int code, const std::string& reason);

  std::string url_;
  std::string ca_path_;
  bool relay_protocol_ = true;
  post_fn post_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> ready_{false};
  std::atomic<bool> closed_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> close_delivered_{false};

  std::mutex send_mutex_;
  std::deque<std::pair<bool, std::string>> sends_;

  std::mutex handler_mutex_;
  text_handler text_;
  binary_handler binary_;
  close_handler close_;
  std::function<void()> ready_handler_;
};

}  // namespace molink
