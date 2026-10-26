#include "transport/relay_ladder.h"

#include <utility>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>

namespace molink {

relay_ladder::relay_ladder(uvcpp::uvcpp_loop* loop,
                           std::vector<factory> factories,
                           int attempt_timeout_ms)
    : loop_(loop),
      factories_(std::move(factories)),
      attempt_timeout_ms_(attempt_timeout_ms) {
  if (loop_ != nullptr) {
    timer_ = std::make_unique<uvcpp::uvcpp_timer>(loop_);
    timer_->init();
  }
}

relay_ladder::~relay_ladder() { close(transport_close::normal, "gone"); }

bool relay_ladder::is_open() const { return ready_ && !closed_; }

void relay_ladder::set_ready_handler(std::function<void()> ready) {
  ready_handler_ = std::move(ready);
  if (ready_ && !closed_ && ready_handler_) {
    std::function<void()> handler = ready_handler_;
    handler();
  }
}

void relay_ladder::set_handlers(text_handler on_text,
                                binary_handler on_binary,
                                close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  on_close_ = std::move(on_close);
}

void relay_ladder::start() {
  if (started_) return;
  started_ = true;
  try_next();
}

void relay_ladder::try_next() {
  if (closed_) return;
  if (next_ >= factories_.size()) {
    deliver_close(transport_close::policy,
                  "all relay endpoints failed");
    return;
  }
  factory create = factories_[next_++];
  if (!create) {
    try_next();
    return;
  }
  active_ = create();
  if (!active_) {
    try_next();
    return;
  }
  install_handlers();
  std::shared_ptr<relay_session> transport = active_;
  transport->set_ready_handler([this] { on_active_ready(); });
  transport->start();
  // The last endpoint has nowhere to fall back to: let it wait for its peer
  // instead of abandoning a slow but valid pairing.
  if (next_ < factories_.size()) start_timer();
}

void relay_ladder::install_handlers() {
  relay_session* current = active_.get();
  active_->set_handlers(
      [this](const std::string& text) {
        if (text_) text_(text);
      },
      [this](const uint8_t* data, std::size_t len) {
        if (binary_) binary_(data, len);
      },
      [this, current](int code, const std::string& reason) {
        // Ignore a late close from an attempt that was already abandoned.
        if (active_.get() != current) return;
        on_active_closed(code, reason);
      });
}

void relay_ladder::on_active_ready() {
  if (closed_) return;
  stop_timer();
  ready_ = true;
  active_protocol_ = active_ ? active_->name() : "relay";
  if (ready_handler_) {
    std::function<void()> handler = ready_handler_;
    handler();
  }
}

void relay_ladder::on_active_closed(int code, const std::string& reason) {
  if (closed_) return;
  if (ready_) {
    deliver_close(code, reason);
    return;
  }
  // Failed before pairing: keep the abandoned transport alive until this
  // callback returns, then move on.
  std::shared_ptr<relay_session> failed = active_;
  active_.reset();
  stop_timer();
  (void)failed;
  try_next();
}

void relay_ladder::start_timer() {
  if (timer_ == nullptr) return;
  timer_->stop();
  timer_->start(
      [this](uvcpp::uvcpp_timer*) {
        if (closed_ || ready_ || !active_) return;
        std::shared_ptr<relay_session> failed = active_;
        active_.reset();
        if (failed) {
          failed->close(transport_close::normal, "relay attempt timed out");
        }
        try_next();
      },
      static_cast<std::uint64_t>(attempt_timeout_ms_), 0);
}

void relay_ladder::stop_timer() {
  if (timer_ != nullptr) timer_->stop();
}

bool relay_ladder::send_text(const std::string& text) {
  if (!ready_ || closed_ || !active_) return false;
  return active_->send_text(text);
}

bool relay_ladder::send_binary(const uint8_t* data, std::size_t len) {
  if (!ready_ || closed_ || !active_) return false;
  return active_->send_binary(data, len);
}

void relay_ladder::close(int code, const std::string& reason) {
  (void)code;
  (void)reason;
  if (closed_) return;
  closed_ = true;
  stop_timer();
  std::shared_ptr<relay_session> transport = active_;
  active_.reset();
  if (transport) {
    transport->close(transport_close::normal, "relay closed");
  }
}

void relay_ladder::deliver_close(int code, const std::string& reason) {
  if (close_delivered_) return;
  close_delivered_ = true;
  closed_ = true;
  stop_timer();
  if (on_close_) {
    close_handler handler = std::move(on_close_);
    on_close_ = nullptr;
    handler(code, reason);
  }
}

}  // namespace molink
