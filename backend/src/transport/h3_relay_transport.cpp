#include "transport/h3_relay_transport.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

#include <uvcpp/uvcpp_config.h>

#ifndef MOLINK_HAVE_H3_WS
#define MOLINK_HAVE_H3_WS 0
#endif

#if MOLINK_HAVE_H3_WS
#include <handle/uvcpp_loop.h>
#include <http3/uvcpp_h3_common.h>
#include <http3/uvcpp_h3_connection.h>
#include <http3/uvcpp_h3_ws_connection.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_connection.h>
#include <ssl/uvcpp_ssl_context.h>
#include <web/uvcpp_ws_frame.h>
#endif

namespace molink {
namespace {
constexpr const char* kRelayH3Path = "/v1/relay";
}

h3_relay_transport::h3_relay_transport(std::string host, int port,
                                       std::string connect_id,
                                       std::string token, std::string ca_path,
                                       uvcpp::uvcpp_loop* loop, post_fn post)
    : host_(std::move(host)),
      port_(port),
      connect_id_(std::move(connect_id)),
      token_(std::move(token)),
      ca_path_(std::move(ca_path)),
      loop_(loop),
      post_(std::move(post)) {}

h3_relay_transport::~h3_relay_transport() {
  closed_ = true;
#if MOLINK_HAVE_H3_WS
  ws_.reset();
  h3_.reset();
  if (quic_ && quic_->connection() != nullptr) {
    quic_->connection()->close();
  }
#endif
}

bool h3_relay_transport::is_open() const {
#if MOLINK_HAVE_H3_WS
  return ready_ && !closed_ && ws_ != nullptr && ws_->is_open();
#else
  return false;
#endif
}

void h3_relay_transport::set_ready_handler(std::function<void()> ready) {
  ready_handler_ = std::move(ready);
  if (ready_ && !closed_ && ready_handler_) {
    std::function<void()> handler = ready_handler_;
    handler();
  }
}

void h3_relay_transport::set_handlers(text_handler on_text,
                                      binary_handler on_binary,
                                      close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  on_close_ = std::move(on_close);
}

void h3_relay_transport::start() {
  if (started_) return;
  started_ = true;
#if !MOLINK_HAVE_H3_WS
  fail("this build has no HTTP/3 WebSocket support");
#else
  tls_ = std::make_shared<uvcpp::uvcpp_ssl_context>(uvcpp::tls_mode::CLIENT);
  if (!ca_path_.empty() && tls_->load_ca_file(ca_path_)) {
    tls_->set_verify_mode(uvcpp::tls_verify_mode::PEER);
  } else {
    tls_->set_verify_mode(uvcpp::tls_verify_mode::NONE);
  }
  tls_->set_alpn_protos({"h3"});
  quic_ = std::make_shared<uvcpp::uvcpp_quic_client>(loop_);
  quic_->set_ssl_context(tls_.get());
  quic_->set_alpn_protos({"h3"});
  std::weak_ptr<h3_relay_transport> weak = weak_from_this();
  const int status = quic_->connect(
      host_.c_str(), port_, [weak](int connect_status) {
        if (auto self = weak.lock()) self->on_connected(connect_status);
      });
  if (status != 0) fail("relay HTTP/3 connect failed to start");
#endif
}

#if MOLINK_HAVE_H3_WS
void h3_relay_transport::on_connected(int status) {
  if (closed_) return;
  uvcpp::uvcpp_quic_connection* connection =
      quic_ ? quic_->connection() : nullptr;
  if (status != 0 || connection == nullptr) {
    fail("relay HTTP/3 handshake failed (" + std::to_string(status) + ")");
    return;
  }
  std::weak_ptr<h3_relay_transport> weak = weak_from_this();
  h3_ = std::make_unique<uvcpp::uvcpp_h3_connection>(connection, false);
  uvcpp::uvcpp_h3_connection::callbacks callbacks;
  callbacks.on_disconnect = [weak](uvcpp::uvcpp_h3_connection&) {
    if (auto self = weak.lock()) {
      self->on_link_closed(transport_close::normal,
                           "relay HTTP/3 connection closed");
    }
  };
  callbacks.on_error = [weak](uvcpp::uvcpp_h3_connection&, int code) {
    if (auto self = weak.lock()) {
      self->on_link_closed(transport_close::policy,
                           "relay HTTP/3 error " + std::to_string(code));
    }
  };
  if (h3_->start(callbacks) != 0) {
    fail("relay HTTP/3 start failed");
    return;
  }

  uvcpp::uvcpp_h3_ws_connection::callbacks ws_callbacks;
  ws_callbacks.on_text = [weak](const std::string& message) {
    if (auto self = weak.lock()) self->on_text(message);
  };
  ws_callbacks.on_binary = [weak](const uint8_t* data, std::size_t len) {
    if (auto self = weak.lock()) self->on_binary(data, len);
  };
  ws_callbacks.on_close = [weak](uvcpp::ws_close_code code,
                                 const std::string& reason) {
    if (auto self = weak.lock()) {
      self->on_link_closed(static_cast<int>(code), reason);
    }
  };
  ws_callbacks.on_error = [weak](int code, const std::string& reason) {
    if (auto self = weak.lock()) self->on_link_closed(code, reason);
  };
  ws_ = uvcpp::uvcpp_h3_ws_connection::connect(*h3_, kRelayH3Path, host_,
                                               ws_callbacks);
  if (!ws_) {
    fail("relay HTTP/3 extended CONNECT failed");
    return;
  }
  nlohmann::json handshake;
  handshake["connect_id"] = connect_id_;
  handshake["token"] = token_;
  const std::string text = handshake.dump();
  ws_->send_text(text.data(), text.size());
}
#else
void h3_relay_transport::on_connected(int status) { (void)status; }
#endif

void h3_relay_transport::on_text(const std::string& text) {
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

void h3_relay_transport::on_binary(const uint8_t* data, std::size_t len) {
  if (closed_ || !ready_) return;
  if (binary_) binary_(data, len);
}

void h3_relay_transport::on_link_closed(int code, const std::string& reason) {
  if (closed_ && close_delivered_) return;
  closed_ = true;
  deliver_close(code, reason);
}

bool h3_relay_transport::send_text(const std::string& text) {
#if MOLINK_HAVE_H3_WS
  if (!is_open()) return false;
  return ws_->send_text(text.data(), text.size()) == 0;
#else
  (void)text;
  return false;
#endif
}

bool h3_relay_transport::send_binary(const uint8_t* data, std::size_t len) {
#if MOLINK_HAVE_H3_WS
  if (!is_open() || (data == nullptr && len > 0)) return false;
  return ws_->send_binary(data, len) == 0;
#else
  (void)data;
  (void)len;
  return false;
#endif
}

void h3_relay_transport::close(int code, const std::string& reason) {
  (void)code;
  (void)reason;
  if (closed_) return;
  closed_ = true;
#if MOLINK_HAVE_H3_WS
  if (ws_) {
    ws_->close(uvcpp::ws_close_code::NORMAL, "relay closed");
  }
#endif
}

void h3_relay_transport::deliver_close(int code, const std::string& reason) {
  if (close_delivered_) return;
  close_delivered_ = true;
  closed_ = true;
  if (on_close_) {
    close_handler handler = std::move(on_close_);
    on_close_ = nullptr;
    handler(code, reason);
  }
}

void h3_relay_transport::fail(const std::string& reason) {
  if (closed_ && close_delivered_) return;
  closed_ = true;
  deliver_close(transport_close::policy, reason);
}

}  // namespace molink
