#include "transport/ws_transport.h"

#include <utility>

#include "diag_log.h"
#include "util.h"

namespace molink {
namespace {

constexpr std::uint64_t kSendFailedLogIntervalMs = 1000;

}  // namespace

ws_transport::ws_transport(uvcpp::uvcpp_ws_connection* conn,
                           std::string peer_ip, unsigned int peer_port)
    : peer_ip_(std::move(peer_ip)), peer_port_(peer_port) {
  link_->conn.store(conn, std::memory_order_release);
  if (conn == nullptr) link_->closed.store(true, std::memory_order_release);
}

ws_transport::~ws_transport() = default;

bool ws_transport::is_open() const {
  // Deliberately does not dereference the connection: this runs from worker
  // posts too, while the app loop may be recycling it. The close latch is the
  // authoritative "can this session still send" bit.
  return !link_->closed.load(std::memory_order_acquire);
}

bool ws_transport::send_text(const std::string& text) {
  return send_bytes(true, text.data(), text.size());
}

bool ws_transport::send_binary(const uint8_t* data, std::size_t len) {
  return send_bytes(false, reinterpret_cast<const char*>(data), len);
}

bool ws_transport::send_bytes(bool text, const char* data, std::size_t len) {
  if (link_->closed.load(std::memory_order_acquire)) return false;

  uvcpp::uvcpp_ws_connection* conn =
      link_->conn.load(std::memory_order_acquire);
  if (conn == nullptr) {
    link_->closed.store(true, std::memory_order_release);
    return false;
  }
  // `send_data` compresses before it looks at the connection state, so a
  // pointer to a retired (or about to be recycled) connection must not reach
  // it. Retired-but-not-recycled is still safe to query; detach right away so
  // this is also the last time we touch it.
  if (!conn->is_open()) {
    detach();
    return false;
  }

  const int rc = text ? conn->send_text(data, len) : conn->send_binary(data, len);
  if (rc < 0) {
    // The connection refused the frame. Do not walk into it again on the next
    // push tick: latch closed and let the session teardown clean up.
    detach();
    log_send_failed(text ? "text" : "binary", rc);
    return false;
  }
  return true;
}

void ws_transport::close(int code, const std::string& reason) {
  // Stop every future send before touching libuvcpp. A locally initiated close
  // does **not** produce an on_close callback, so this is the only place that
  // can drop the pointer on this path.
  link_->closed.store(true, std::memory_order_release);
  uvcpp::uvcpp_ws_connection* conn =
      link_->conn.exchange(nullptr, std::memory_order_acq_rel);
  if (conn == nullptr) return;
  conn->close(static_cast<uvcpp::ws_close_code>(code), reason);
}

void ws_transport::detach() {
  // Drop the pointer first: a concurrent reader that checks the latch after
  // loading the pointer sees a null in either order.
  link_->conn.store(nullptr, std::memory_order_release);
  link_->closed.store(true, std::memory_order_release);
}

void ws_transport::log_send_failed(const char* kind, int rc) {
  const std::uint64_t now = now_ms();
  std::uint64_t last =
      last_send_failed_log_ms_.load(std::memory_order_relaxed);
  if (last != 0 && now >= last && now - last < kSendFailedLogIntervalMs) {
    return;
  }
  last_send_failed_log_ms_.store(now, std::memory_order_relaxed);
  diag().log(diag_level::warn, "transport.send_failed", "websocket send rejected",
             {{"peer", peer_ip_},
              {"kind", kind},
              {"rc", std::to_string(rc)}});
}

void ws_transport::set_handlers(text_handler on_text, binary_handler on_binary,
                                close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  close_ = std::move(on_close);

  uvcpp::uvcpp_ws_connection* conn =
      link_->conn.load(std::memory_order_acquire);
  if (conn == nullptr || link_->closed.load(std::memory_order_acquire)) return;

  // Capture a weak reference, never `this`: libuvcpp keeps these closures on
  // the connection, and the connection is recycled on a later loop turn --
  // possibly after the session (and this wrapper) has gone away. Locking makes
  // a late callback a no-op instead of a use-after-free.
  std::weak_ptr<ws_transport> weak = weak_from_this();
  conn->on_text([weak](const std::string& text) {
    if (auto self = weak.lock()) {
      if (self->text_) self->text_(text);
    }
  });
  conn->on_binary([weak](const uint8_t* data, std::size_t len) {
    if (auto self = weak.lock()) {
      if (self->binary_) self->binary_(data, len);
    }
  });
  conn->on_close([weak](uvcpp::ws_close_code code, const std::string& reason) {
    if (auto self = weak.lock()) {
      // Keep `self` alive across the user handler: the handler normally drops
      // the last session reference, which destroys the controller that owns
      // this wrapper. The local shared_ptr is what lets us finish the call
      // safely.
      self->link_->closed.store(true, std::memory_order_release);
      self->link_->conn.store(nullptr, std::memory_order_release);
      close_handler handler = std::move(self->close_);
      self->text_ = nullptr;
      self->binary_ = nullptr;
      if (handler) handler(static_cast<int>(code), reason);
    }
  });
  // A protocol error also terminates the connection, but libuvcpp reports it
  // through on_error and deliberately suppresses on_close (the close
  // notification is "how the peer left", not "we rejected its frame"). Route
  // it through the same close handler so the session is torn down instead of
  // lingering until the idle reaper.
  conn->on_error([weak](int code, const std::string& reason) {
    if (auto self = weak.lock()) {
      self->link_->closed.store(true, std::memory_order_release);
      self->link_->conn.store(nullptr, std::memory_order_release);
      close_handler handler = std::move(self->close_);
      self->text_ = nullptr;
      self->binary_ = nullptr;
      if (handler) handler(code, reason);
    }
  });

  // The TCP client is alive for as long as the session is; installing an
  // observer here covers every close path, including those that libuvcpp
  // initiates itself (server shutdown) and therefore does not report through
  // on_close. The observer only touches the shared state, so it is safe even
  // if this wrapper has already been destroyed.
  std::shared_ptr<link_state> link = link_;
  if (uvcpp::uvcpp_tcp_client* tcp = conn->get_tcp_client()) {
    tcp->add_close_observer([link]() {
      link->conn.store(nullptr, std::memory_order_release);
      link->closed.store(true, std::memory_order_release);
    });
  }
  // The upgrade may already have retired the connection before the handler
  // ran (the close observer above only fires from now on).
  if (!conn->is_open()) detach();
}

}  // namespace molink
