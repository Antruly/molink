#include "transport/h2_ws_transport.h"

#include <utility>

#ifndef MOLINK_HAVE_H2_WS
#define MOLINK_HAVE_H2_WS 0
#endif

#if MOLINK_HAVE_H2_WS
#include <http2/uvcpp_h2_ws_connection.h>
#include <web/uvcpp_ws_frame.h>
#endif

namespace molink {

h2_ws_transport::h2_ws_transport(std::string peer_ip, unsigned int peer_port)
    : peer_ip_(std::move(peer_ip)), peer_port_(peer_port) {}

h2_ws_transport::~h2_ws_transport() = default;

bool h2_ws_transport::is_open() const {
#if MOLINK_HAVE_H2_WS
  return !closed_ && ws_ != nullptr && ws_->is_open();
#else
  return false;
#endif
}

bool h2_ws_transport::send_text(const std::string& text) {
#if MOLINK_HAVE_H2_WS
  if (!is_open()) return false;
  return ws_->send_text(text.data(), text.size()) == 0;
#else
  (void)text;
  return false;
#endif
}

bool h2_ws_transport::send_binary(const uint8_t* data, std::size_t len) {
#if MOLINK_HAVE_H2_WS
  if (!is_open() || (data == nullptr && len > 0)) return false;
  return ws_->send_binary(data, len) == 0;
#else
  (void)data;
  (void)len;
  return false;
#endif
}

void h2_ws_transport::close(int code, const std::string& reason) {
#if MOLINK_HAVE_H2_WS
  if (closed_) return;
  closed_ = true;
  if (ws_ != nullptr) {
    uvcpp::uvcpp_h2_ws_connection* ws = ws_;
    ws_ = nullptr;
    ws->close(static_cast<uvcpp::ws_close_code>(code), reason);
  }
#else
  (void)code;
  (void)reason;
#endif
}

void h2_ws_transport::set_handlers(text_handler on_text,
                                   binary_handler on_binary,
                                   close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  on_close_ = std::move(on_close);
}

#if MOLINK_HAVE_H2_WS
void h2_ws_transport::attach(uvcpp::uvcpp_h2_ws_connection* ws) {
  ws_ = ws;
}

void h2_ws_transport::deliver_text(const std::string& text) {
  if (!closed_ && text_) text_(text);
}

void h2_ws_transport::deliver_binary(const uint8_t* data, std::size_t len) {
  if (!closed_ && binary_) binary_(data, len);
}

void h2_ws_transport::link_closed(int code, const std::string& reason) {
  if (close_delivered_) return;
  close_delivered_ = true;
  closed_ = true;
  ws_ = nullptr;
  if (on_close_) {
    close_handler handler = std::move(on_close_);
    on_close_ = nullptr;
    handler(code, reason);
  }
}
#endif

}  // namespace molink
