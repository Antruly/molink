#include "transport/h2_relay_transport.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

#include <uvcpp/uvcpp_config.h>

#ifndef MOLINK_HAVE_H2_WS
#define MOLINK_HAVE_H2_WS 0
#endif

#if MOLINK_HAVE_H2_WS
#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <http2/uvcpp_h2_connection.h>
#include <http2/uvcpp_h2_ws_connection.h>
#include <net/uvcpp_tcp_client.h>
#include <web/uvcpp_ws_frame.h>
#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif
#endif

namespace molink {
namespace {
constexpr const char* kRelayPath = "/v1/relay";
}

h2_relay_transport::h2_relay_transport(std::string host, int port,
                                       std::string connect_id,
                                       std::string token, std::string ca_path,
                                       bool secure, uvcpp::uvcpp_loop* loop,
                                       post_fn post)
    : host_(std::move(host)),
      port_(port),
      connect_id_(std::move(connect_id)),
      token_(std::move(token)),
      ca_path_(std::move(ca_path)),
      secure_(secure),
      loop_(loop),
      post_(std::move(post)) {}

h2_relay_transport::~h2_relay_transport() {
  closed_ = true;
#if MOLINK_HAVE_H2_WS
  ws_.reset();
  h2_.reset();
  if (tcp_) tcp_->close();
  tcp_.reset();
#endif
}

bool h2_relay_transport::is_open() const {
#if MOLINK_HAVE_H2_WS
  return !closed_ && ready_ && ws_ != nullptr && ws_->is_open();
#else
  return false;
#endif
}

void h2_relay_transport::set_ready_handler(std::function<void()> ready) {
  ready_handler_ = std::move(ready);
  if (ready_ && !closed_ && ready_handler_) {
    std::function<void()> handler = ready_handler_;
    handler();
  }
}

void h2_relay_transport::set_handlers(text_handler on_text,
                                      binary_handler on_binary,
                                      close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  on_close_ = std::move(on_close);
}

void h2_relay_transport::start() {
  if (started_ || closed_) return;
  started_ = true;
#if !MOLINK_HAVE_H2_WS
  fail("this build has no HTTP/2 WebSocket support");
#else
  if (loop_ == nullptr) {
    fail("relay HTTP/2 needs an event loop");
    return;
  }
  tcp_ = std::make_unique<uvcpp::uvcpp_tcp_client>(loop_);
#if UVCPP_OPENSSL_ENABLE
  if (secure_) {
    tls_ = std::make_unique<uvcpp::uvcpp_ssl_context>(
        uvcpp::tls_mode::CLIENT);
    if (!ca_path_.empty() && tls_->load_ca_file(ca_path_)) {
      tls_->set_verify_mode(uvcpp::tls_verify_mode::PEER);
    } else {
      tls_->set_verify_mode(uvcpp::tls_verify_mode::NONE);
    }
    tls_->set_alpn_protos({"h2"});
    if (tcp_->enable_tls(tls_.get()) != 0) {
      fail("relay HTTP/2 TLS setup failed");
      return;
    }
  }
#else
  if (secure_) {
    fail("relay HTTP/2 needs TLS, but this build has no OpenSSL");
    return;
  }
#endif
  deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  std::weak_ptr<h2_relay_transport> weak = weak_from_this();
  const int status = tcp_->connect(
      host_.c_str(), port_, [weak](int connect_status) {
        if (auto self = weak.lock()) self->on_connected(connect_status);
      });
  if (status != 0) fail("relay HTTP/2 connect failed to start");
#endif
}

#if MOLINK_HAVE_H2_WS
void h2_relay_transport::on_connected(int status) {
  if (closed_ || ws_) return;
  if (status != 0 || !tcp_) {
    fail("relay HTTP/2 TCP connect failed (" + std::to_string(status) + ")");
    return;
  }
  h2_ = std::make_unique<uvcpp::uvcpp_h2_connection>(tcp_.get(),
                                                     /*server_side=*/false);
  uvcpp::uvcpp_h2_session::callbacks h2_callbacks;
  uvcpp::uvcpp_h2_connection::callbacks connection_callbacks;
  std::weak_ptr<h2_relay_transport> weak = weak_from_this();
  connection_callbacks.on_disconnect = [weak](uvcpp::uvcpp_h2_connection&) {
    if (auto self = weak.lock()) {
      self->on_link_closed(transport_close::normal,
                           "relay HTTP/2 connection closed");
    }
  };
  if (h2_->start(h2_callbacks, connection_callbacks) != 0) {
    fail("relay HTTP/2 session start failed");
    return;
  }
  timer_ = std::make_unique<uvcpp::uvcpp_timer>(loop_);
  if (timer_->init() != 0) {
    fail("relay HTTP/2 timer init failed");
    return;
  }
  timer_->start(
      [weak](uvcpp::uvcpp_timer*) {
        if (auto self = weak.lock()) self->try_connect();
      },
      20, 20);
}

void h2_relay_transport::try_connect() {
  if (closed_ || ws_ != nullptr || !h2_) return;
  if (std::chrono::steady_clock::now() > deadline_) {
    fail("relay HTTP/2 handshake timed out");
    return;
  }
  std::weak_ptr<h2_relay_transport> weak = weak_from_this();
  uvcpp::uvcpp_h2_ws_connection::callbacks callbacks;
  callbacks.on_text = [weak](const std::string& text) {
    if (auto self = weak.lock()) self->on_text(text);
  };
  callbacks.on_binary = [weak](const uint8_t* data, std::size_t len) {
    if (auto self = weak.lock()) self->on_binary(data, len);
  };
  callbacks.on_close = [weak](uvcpp::ws_close_code code,
                              const std::string& reason) {
    if (auto self = weak.lock()) {
      self->on_link_closed(static_cast<int>(code), reason);
    }
  };
  callbacks.on_error = [weak](int code, const std::string& reason) {
    if (auto self = weak.lock()) self->on_link_closed(code, reason);
  };
  auto ws = uvcpp::uvcpp_h2_ws_connection::connect(*h2_, kRelayPath, host_,
                                                   callbacks);
  if (ws == nullptr) return;  // SETTINGS not received yet; retry.
  if (timer_) {
    timer_->stop();
    timer_.reset();
  }
  ws_ = std::move(ws);
  nlohmann::json handshake;
  handshake["connect_id"] = connect_id_;
  handshake["token"] = token_;
  const std::string text = handshake.dump();
  ws_->send_text(text.data(), text.size());
}

void h2_relay_transport::on_text(const std::string& text) {
  if (closed_) return;
  if (!ready_) {
    nlohmann::json body;
    try {
      body = nlohmann::json::parse(text);
    } catch (const std::exception&) {
      return;
    }
    if (!body.is_object()) return;
    const std::string type = body.value("type", std::string());
    if (type == "relay-ready") {
      ready_ = true;
      if (ready_handler_) {
        std::function<void()> handler = ready_handler_;
        handler();
      }
      return;
    }
    if (type == "relay-error") {
      fail(body.value("message", std::string("relay refused the session")));
    }
    return;
  }
  if (text_) text_(text);
}

void h2_relay_transport::on_binary(const uint8_t* data, std::size_t len) {
  if (closed_ || !ready_) return;
  if (binary_) binary_(data, len);
}

void h2_relay_transport::on_link_closed(int code, const std::string& reason) {
  if (close_delivered_) return;
  closed_ = true;
  if (on_close_) {
    close_handler handler = std::move(on_close_);
    on_close_ = nullptr;
    close_delivered_ = true;
    handler(code, reason);
  }
}
#endif

bool h2_relay_transport::send_text(const std::string& text) {
#if MOLINK_HAVE_H2_WS
  if (!is_open()) return false;
  return ws_->send_text(text.data(), text.size()) == 0;
#else
  (void)text;
  return false;
#endif
}

bool h2_relay_transport::send_binary(const uint8_t* data, std::size_t len) {
#if MOLINK_HAVE_H2_WS
  if (!is_open() || (data == nullptr && len > 0)) return false;
  return ws_->send_binary(data, len) == 0;
#else
  (void)data;
  (void)len;
  return false;
#endif
}

void h2_relay_transport::close(int code, const std::string& reason) {
  (void)code;
  (void)reason;
  if (closed_) return;
  closed_ = true;
#if MOLINK_HAVE_H2_WS
  if (ws_) ws_->close(uvcpp::ws_close_code::NORMAL, "relay closed");
#endif
  stop_connecting();
}

void h2_relay_transport::fail(const std::string& reason) {
  if (close_delivered_) return;
  closed_ = true;
  if (on_close_) {
    close_handler handler = std::move(on_close_);
    on_close_ = nullptr;
    close_delivered_ = true;
    handler(transport_close::policy, reason);
  }
  stop_connecting();
}

void h2_relay_transport::stop_connecting() {
#if MOLINK_HAVE_H2_WS
  if (timer_) {
    timer_->stop();
    timer_.reset();
  }
  if (tcp_) tcp_->close();
#endif
}

}  // namespace molink
