#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "transport/relay_session.h"

namespace uvcpp {
class uvcpp_loop;
class uvcpp_timer;
}

namespace molink {

/// Tries a relay endpoint at a time, in preference order, until one pairs.
///
/// The directory lists quic, tcp and websocket endpoints; a firewall or a
/// stopped listener can make the preferred one fail before `relay-ready`.
/// Each attempt gets a deadline, and a failure (or timeout) moves to the next
/// endpoint while the session is still unauthenticated. Once a transport is
/// ready it becomes the active one and later failures are reported normally
/// (ICE may still be delivering the session in parallel).
///
/// Threading: app loop only, like the transports it wraps.
class relay_ladder final : public relay_session {
 public:
  using factory = std::function<std::shared_ptr<relay_session>()>;

  /// `loop` drives the per-attempt timer; nullptr disables it (tests).
  relay_ladder(uvcpp::uvcpp_loop* loop, std::vector<factory> factories,
               int attempt_timeout_ms = 6000);
  ~relay_ladder() override;

  const char* name() const override { return active_protocol_.c_str(); }
  const char* path() const override { return "relay"; }
  std::string peer_ip() const override { return "relay"; }
  unsigned int peer_port() const override { return 0; }

  bool is_open() const override;
  bool send_text(const std::string& text) override;
  bool send_binary(const uint8_t* data, std::size_t len) override;
  void close(int code, const std::string& reason) override;
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override;
  void set_ready_handler(std::function<void()> ready) override;
  void start() override;

 private:
  void try_next();
  void install_handlers();
  void on_active_ready();
  void on_active_closed(int code, const std::string& reason);
  void start_timer();
  void stop_timer();
  void deliver_close(int code, const std::string& reason);
  /// Parks an abandoned transport until the next loop tick.
  void retire(std::shared_ptr<relay_session> transport);

  uvcpp::uvcpp_loop* loop_;
  std::vector<factory> factories_;
  std::size_t next_ = 0;
  int attempt_timeout_ms_;
  std::string active_protocol_ = "relay";
  std::shared_ptr<relay_session> active_;
  /// An abandoned transport is usually reported dead **from inside its own data
  /// callback** (a `relay-error` text frame). Dropping the last reference there
  /// frees it -- and its WebSocket parser -- while that parser is still on the
  /// stack; the next frame in the same read then runs on freed memory
  /// (`trampoline_frame` with a null argument, 2026-10-29). Park it here and
  /// release it on the next tick, outside every callback.
  std::vector<std::shared_ptr<relay_session>> retired_;
  std::unique_ptr<uvcpp::uvcpp_timer> timer_;
  /// Declared after `retired_` so it is destroyed before it: a pending retire
  /// can never outlive the vector it clears.
  std::unique_ptr<uvcpp::uvcpp_timer> retire_timer_;

  bool started_ = false;
  bool ready_ = false;
  bool closed_ = false;
  bool close_delivered_ = false;
  /// The destructor is running: park transports, never arm the retire timer.
  bool destroying_ = false;

  text_handler text_;
  binary_handler binary_;
  close_handler on_close_;
  std::function<void()> ready_handler_;
};

}  // namespace molink
