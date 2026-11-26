#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <web/uvcpp_ws_connection.h>
#include <webapp/uvcpp_web_ws.h>

#include "session_controller.h"
#include "transport/session_transport.h"

namespace molink {

/// Handler for WS /ws/session. The adapter wraps the connection in
/// ws_transport and hands it to session_controller, which owns all session
/// logic.
uvcpp::uvcpp_web_ws_handler make_ws_handler(
    std::shared_ptr<session_registry> registry, session_options options);

/// WebSocket implementation of session_transport. The connection is owned by
/// libuvcpp; this wrapper only borrows it while the session is alive.
///
/// ## Connection lifetime
///
/// libuvcpp recycles a `uvcpp_ws_connection` on a later loop turn after it has
/// retired it. A raw pointer left in this wrapper is therefore a use-after-free
/// as soon as the peer disappears, and `send_binary` compresses *before* it
/// checks whether the connection is retired -- so a stale pointer crashes
/// inside zlib, not at an easily recognisable null check.
///
/// Two independent guards keep that from happening:
///
///  * the underlying TCP client's close observer is installed here. It fires
///    for every close path (peer FIN/RST, our `close()`, the framework closing
///    the connection) and clears the pointer **before** libuvcpp deletes the
///    connection object;
///  * `close()` detaches immediately, because libuvcpp deliberately does not
///    call `on_close` for a close initiated by this side.
///
/// Every send follows the same order: closed latch, load the pointer, ask the
/// connection whether it is open, then send -- all on the app loop thread, so
/// nothing can retire between the check and the call. The atomics exist for
/// the readers on other threads (`is_open()` from worker posts), not to make
/// cross-thread sends legal.
class ws_transport final
    : public session_transport,
      public std::enable_shared_from_this<ws_transport> {
 public:
  ws_transport(uvcpp::uvcpp_ws_connection* conn, std::string peer_ip,
               unsigned int peer_port);
  ~ws_transport() override;

  const char* name() const override { return "websocket"; }
  std::string peer_ip() const override { return peer_ip_; }
  unsigned int peer_port() const override { return peer_port_; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;

 private:
  /// State shared with the TCP close observer. The observer outlives this
  /// wrapper (it belongs to the client), so it must not capture `this`.
  struct link_state {
    std::atomic<uvcpp::uvcpp_ws_connection*> conn{nullptr};
    std::atomic<bool> closed{false};
  };

  bool send_bytes(bool text, const char* data, std::size_t len);
  /// Latches the link closed and drops the connection pointer.
  void detach();
  /// Reports a send that the connection refused. Throttled: a broken link
  /// stays broken, and the frame clock runs many times a second.
  void log_send_failed(const char* kind, int rc);

  std::shared_ptr<link_state> link_ = std::make_shared<link_state>();
  std::string peer_ip_;
  unsigned int peer_port_ = 0;

  text_handler text_;
  binary_handler binary_;
  close_handler close_;
  /// Last `transport.send_failed` line (monotonic ms).
  std::atomic<std::uint64_t> last_send_failed_log_ms_{0};
};

}  // namespace molink
