#include "transport/connect_proxy.h"

#include "transport/switchable_transport.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace molink {
namespace {

class connect_proxy final
    : public std::enable_shared_from_this<connect_proxy> {
 public:
  connect_proxy(uvcpp::uvcpp_ws_connection* browser,
                std::shared_ptr<p2p_manager> manager, std::string target,
                bool relay_only)
      : browser_(browser), manager_(std::move(manager)),
        target_(std::move(target)), relay_only_(relay_only) {}

  void start() {
    if (browser_ == nullptr) return;
    browser_->set_max_message_size(1024u * 1024u);
    // The browser connection is the owner of this proxy: its callbacks keep a
    // strong reference until the WebSocket is gone. The ICE/transport
    // callbacks stay weak so proxy -> transport -> callback is not a cycle.
    std::shared_ptr<connect_proxy> self = shared_from_this();
    browser_->on_text([self](const std::string& text) {
      self->on_browser_text(text);
    });
    browser_->on_binary([self](const uint8_t* data, std::size_t len) {
      self->on_browser_binary(data, len);
    });
    browser_->on_close(
        [self](uvcpp::ws_close_code code, const std::string& reason) {
          self->on_browser_close(code, reason);
        });
    manager_->connect(
        target_, relay_only_,
        [self](std::shared_ptr<session_transport> transport) {
          self->on_ready(std::move(transport));
        },
        [self](const std::string& error) { self->on_error(error); });
  }

 private:
  void on_browser_text(const std::string& text) {
    if (closed_) return;
    if (text.find("\"hello\"") != std::string::npos) {
      // Keep the hello so it can be replayed if the first datagram raced the
      // target's session controller and was dropped.
      hello_text_ = text;
    }
    if (!transport_ || !transport_->is_open()) {
      pending_text_.push_back(text);
      return;
    }
    transport_->send_text(text);
  }

  void on_browser_binary(const uint8_t* data, std::size_t len) {
    if (closed_ || data == nullptr) return;
    if (!transport_ || !transport_->is_open()) {
      pending_binary_.emplace_back(data, data + len);
      return;
    }
    transport_->send_binary(data, len);
  }

  void on_browser_close(uvcpp::ws_close_code code, const std::string& reason) {
    (void)code;
    (void)reason;
    browser_ = nullptr;
    closed_ = true;
    pending_text_.clear();
    pending_binary_.clear();
    if (transport_) transport_->close(transport_close::normal, "browser closed");
    transport_.reset();
  }

  void flush_pending() {
    if (flushed_ || !transport_ || !transport_->is_open()) return;
    flushed_ = true;
    for (const std::string& text : pending_text_) transport_->send_text(text);
    for (const std::vector<uint8_t>& data : pending_binary_) {
      transport_->send_binary(data.data(), data.size());
    }
    pending_text_.clear();
    pending_binary_.clear();
  }

  void send_transport(const char* mode) {
    if (browser_ == nullptr) return;
    const std::string message = std::string("{\"type\":\"transport\",\"mode\":\"") +
                                mode + "\"}";
    browser_->send_text(message.data(), message.size());
  }

  void on_ready(std::shared_ptr<session_transport> transport) {
    if (closed_ || browser_ == nullptr) {
      transport->close(transport_close::normal, "browser closed");
      return;
    }
    transport_ = std::move(transport);
    if (auto switcher =
            std::dynamic_pointer_cast<switchable_transport>(transport_)) {
      std::weak_ptr<connect_proxy> proxy = weak_from_this();
      switcher->set_switch_handler([proxy] {
        if (auto self = proxy.lock()) self->send_transport("p2p");
      });
      send_transport("relay");
    } else {
      send_transport("p2p");
    }
    std::weak_ptr<connect_proxy> weak = weak_from_this();
    transport_->set_handlers(
        [weak](const std::string& text) {
          if (auto self = weak.lock()) {
            self->flush_pending();
            if (text.find("hello_required") != std::string::npos &&
                !self->hello_resent_ && !self->hello_text_.empty()) {
              // The remote only asks for hello after its transport is ready;
              // replay ours in case the first copy arrived too early.
              self->transport_->send_text(self->hello_text_);
              self->hello_resent_ = true;
            }
            if (self->browser_ != nullptr) {
              self->browser_->send_text(text.data(), text.size());
            }
          }
        },
        [weak](const uint8_t* data, std::size_t len) {
          if (auto self = weak.lock()) {
            self->flush_pending();
            if (self->browser_ != nullptr) {
              self->browser_->send_binary(
                  reinterpret_cast<const char*>(data), len);
            }
          }
        },
        [weak](int code, const std::string& reason) {
          if (auto self = weak.lock()) {
            self->closed_ = true;
            if (self->browser_ != nullptr) {
              self->browser_->close(static_cast<uvcpp::ws_close_code>(code),
                                    reason);
            }
            self->browser_ = nullptr;
          }
        });
    // The target controller is created as soon as its ICE pair completes and
    // is already waiting for hello, so send the buffered hello now.
    flush_pending();
  }

  void on_error(const std::string& error) {
    if (closed_ || browser_ == nullptr) return;
    closed_ = true;
    browser_->close(uvcpp::ws_close_code::POLICY_VIOLATION,
                    error.empty() ? "p2p unavailable" : error);
    browser_ = nullptr;
  }

  uvcpp::uvcpp_ws_connection* browser_ = nullptr;
  std::shared_ptr<p2p_manager> manager_;
  std::string target_;
  bool relay_only_ = false;
  std::shared_ptr<session_transport> transport_;
  std::deque<std::string> pending_text_;
  std::deque<std::vector<uint8_t>> pending_binary_;
  std::string hello_text_;
  bool hello_resent_ = false;
  bool flushed_ = false;
  bool closed_ = false;
};

}  // namespace

uvcpp::uvcpp_web_ws_handler make_connect_handler(
    std::shared_ptr<p2p_manager> manager) {
  return [manager](uvcpp::uvcpp_web_ws_request& request) {
    uvcpp::uvcpp_ws_connection* conn = request.connection();
    if (conn == nullptr) return;
    const std::string* target = request.query("target");
    if (target == nullptr || target->empty()) {
      conn->close(uvcpp::ws_close_code::POLICY_VIOLATION,
                  "target device id is required");
      return;
    }
    const std::string* relay = request.query("relay");
    const bool relay_only =
        relay != nullptr && (*relay == "only" || *relay == "1");
    auto proxy =
        std::make_shared<connect_proxy>(conn, manager, *target, relay_only);
    proxy->start();
  };
}

}  // namespace molink
