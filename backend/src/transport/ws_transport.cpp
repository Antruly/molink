#include "transport/ws_transport.h"

#include <utility>

namespace molink {

ws_transport::ws_transport(uvcpp::uvcpp_ws_connection* conn,
                           std::string peer_ip, unsigned int peer_port)
    : conn_(conn), peer_ip_(std::move(peer_ip)), peer_port_(peer_port) {}

bool ws_transport::is_open() const {
  return conn_ != nullptr && conn_->is_open();
}

bool ws_transport::send_text(const std::string& text) {
  if (!is_open()) return false;
  return conn_->send_text(text.data(), text.size()) >= 0;
}

bool ws_transport::send_binary(const uint8_t* data, std::size_t len) {
  if (!is_open()) return false;
  return conn_->send_binary(reinterpret_cast<const char*>(data), len) >= 0;
}

void ws_transport::close(int code, const std::string& reason) {
  if (conn_ == nullptr) return;
  conn_->close(static_cast<uvcpp::ws_close_code>(code), reason);
}

void ws_transport::set_handlers(text_handler on_text, binary_handler on_binary,
                                close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  close_ = std::move(on_close);
  if (conn_ == nullptr) return;

  conn_->on_text([this](const std::string& text) {
    if (text_) text_(text);
  });
  conn_->on_binary([this](const uint8_t* data, std::size_t len) {
    if (binary_) binary_(data, len);
  });
  conn_->on_close([this](uvcpp::ws_close_code code, const std::string& reason) {
    // Move the handler out and drop every reference before invoking it, so the
    // controller -> transport -> handler cycle is broken exactly once.
    close_handler handler = std::move(close_);
    text_ = nullptr;
    binary_ = nullptr;
    conn_ = nullptr;
    if (handler) handler(static_cast<int>(code), reason);
  });
}

}  // namespace molink
