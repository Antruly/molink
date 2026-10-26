#include "transport/web_ws_transport.h"

#include <utility>

#ifndef MOLINK_HAVE_H2_WS
#define MOLINK_HAVE_H2_WS 0
#endif

#if MOLINK_HAVE_H2_WS
#include <webapp/uvcpp_web_ws.h>
#endif

namespace molink {

web_ws_transport::web_ws_transport(std::string protocol)
    : protocol_(std::move(protocol)) {}

web_ws_transport::~web_ws_transport() = default;

void web_ws_transport::set_peer(std::string ip, unsigned int port) {
  peer_ip_ = std::move(ip);
  peer_port_ = port;
}

bool web_ws_transport::is_open() const {
#if MOLINK_HAVE_H2_WS
  return !closed_ && session_ != nullptr && session_->is_open();
#else
  return false;
#endif
}

bool web_ws_transport::send_text(const std::string& text) {
#if MOLINK_HAVE_H2_WS
  if (!is_open()) return false;
  return session_->send_text(text.data(), text.size()) == 0;
#else
  (void)text;
  return false;
#endif
}

bool web_ws_transport::send_binary(const uint8_t* data, std::size_t len) {
#if MOLINK_HAVE_H2_WS
  if (!is_open() || (data == nullptr && len > 0)) return false;
  return session_->send_binary(data, len) == 0;
#else
  (void)data;
  (void)len;
  return false;
#endif
}

void web_ws_transport::close(int code, const std::string& reason) {
#if MOLINK_HAVE_H2_WS
  if (closed_) return;
  closed_ = true;
  if (session_ != nullptr) {
    uvcpp::uvcpp_web_ws_session* session = session_;
    session_ = nullptr;
    session->close(static_cast<uvcpp::ws_close_code>(code), reason);
  }
#else
  (void)code;
  (void)reason;
#endif
}

void web_ws_transport::set_handlers(text_handler on_text,
                                    binary_handler on_binary,
                                    close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  on_close_ = std::move(on_close);
}

#if MOLINK_HAVE_H2_WS
void web_ws_transport::attach(uvcpp::uvcpp_web_ws_session* session) {
  session_ = session;
  if (session == nullptr) return;
  std::weak_ptr<web_ws_transport> weak = weak_from_this();
  session->on_text([weak](const std::string& text) {
    if (auto self = weak.lock()) self->deliver_text(text);
  });
  session->on_binary([weak](const uint8_t* data, std::size_t len) {
    if (auto self = weak.lock()) self->deliver_binary(data, len);
  });
  session->on_close([weak](uvcpp::ws_close_code code,
                           const std::string& reason) {
    if (auto self = weak.lock()) {
      self->link_closed(static_cast<int>(code), reason);
    }
  });
  session->on_error([weak](int code, const std::string& reason) {
    if (auto self = weak.lock()) self->link_closed(code, reason);
  });
}

void web_ws_transport::deliver_text(const std::string& text) {
  if (!closed_ && text_) text_(text);
}

void web_ws_transport::deliver_binary(const uint8_t* data, std::size_t len) {
  if (!closed_ && binary_) binary_(data, len);
}

void web_ws_transport::link_closed(int code, const std::string& reason) {
  if (close_delivered_) return;
  close_delivered_ = true;
  closed_ = true;
  session_ = nullptr;
  if (on_close_) {
    close_handler handler = std::move(on_close_);
    on_close_ = nullptr;
    handler(code, reason);
  }
}
#endif

}  // namespace molink
