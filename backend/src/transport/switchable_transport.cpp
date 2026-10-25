#include "transport/switchable_transport.h"

#include <utility>

namespace molink {

switchable_transport::switchable_transport(
    std::shared_ptr<session_transport> initial)
    : active_(std::move(initial)), relay_(active_) {
  wire(relay_);
}

const char* switchable_transport::name() const {
  return active_ ? active_->name() : "relay";
}

std::string switchable_transport::peer_ip() const {
  return active_ ? active_->peer_ip() : std::string("relay");
}

unsigned int switchable_transport::peer_port() const {
  return active_ ? active_->peer_port() : 0;
}

bool switchable_transport::is_open() const {
  return !closed_ && active_ && active_->is_open();
}

bool switchable_transport::send_text(const std::string& text) {
  return active_ && active_->send_text(text);
}

bool switchable_transport::send_binary(const uint8_t* data, std::size_t len) {
  return active_ && active_->send_binary(data, len);
}

void switchable_transport::close(int code, const std::string& reason) {
  if (closed_) return;
  closed_ = true;
  if (relay_) relay_->close(code, reason);
  if (ice_) ice_->close(code, reason);
  active_.reset();
}

void switchable_transport::set_handlers(text_handler on_text,
                                        binary_handler on_binary,
                                        close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  close_ = std::move(on_close);
  if (relay_) wire(relay_);
  if (ice_) wire(ice_);
}

void switchable_transport::wire(
    const std::shared_ptr<session_transport>& transport) {
  if (!transport) return;
  auto source = transport.get();
  transport->set_handlers(
      [this, source](const std::string& text) {
        if (closed_ || active_.get() != source || !text_) return;
        text_(text);
      },
      [this, source](const uint8_t* data, std::size_t len) {
        if (closed_ || active_.get() != source || !binary_) return;
        binary_(data, len);
      },
      [this, source](int code, const std::string& reason) {
        if (closed_ || active_.get() != source || !close_) return;
        closed_ = true;
        close_(code, reason);
      });
}

void switchable_transport::set_switch_handler(std::function<void()> handler) {
  switch_handler_ = std::move(handler);
}

void switchable_transport::set_ice(
    std::shared_ptr<session_transport> ice) {
  if (!ice || switched_ || closed_) return;
  ice_ = std::move(ice);
  wire(ice_);
  switched_ = true;
  active_ = ice_;
  // The relay is no longer read; close it so the server frees the pair.
  if (relay_) {
    std::shared_ptr<session_transport> old = relay_;
    relay_.reset();
    old->close(transport_close::normal, "switched to p2p");
  }
  if (switch_handler_) switch_handler_();
}

}  // namespace molink
