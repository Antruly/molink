#include "transport/relay_stream_transport.h"

#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <handle/uvcpp_loop.h>
#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uvcpp/uvcpp_config.h>

#include "protocol/frame_packet.h"

namespace molink {
namespace {

constexpr std::size_t kMaxBacklogBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxHandshakeBytes = 4096;

}  // namespace

relay_stream_transport::relay_stream_transport(
    std::string protocol, std::string host, int port,
    std::string connect_id, std::string token, std::string ca_path,
    bool secure, uvcpp::uvcpp_loop* loop, post_fn post)
    : protocol_(std::move(protocol)),
      host_(std::move(host)),
      port_(port),
      connect_id_(std::move(connect_id)),
      token_(std::move(token)),
      ca_path_(std::move(ca_path)),
      secure_(secure),
      loop_(loop),
      post_(std::move(post)) {}

relay_stream_transport::~relay_stream_transport() {
  closed_ = true;
  if (client_) client_->close();
}

bool relay_stream_transport::is_open() const { return ready_ && !closed_; }

void relay_stream_transport::set_ready_handler(std::function<void()> ready) {
  ready_handler_ = std::move(ready);
  if (ready_ && ready_handler_) {
    std::function<void()> handler = ready_handler_;
    handler();
  }
}

void relay_stream_transport::set_handlers(text_handler on_text,
                                          binary_handler on_binary,
                                          close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  on_close_ = std::move(on_close);
}

void relay_stream_transport::start() {
  if (started_) return;
  started_ = true;
  if (secure_) {
    tls_ = std::make_shared<uvcpp::uvcpp_ssl_context>(uvcpp::tls_mode::CLIENT);
    // PEER (chain only): the directory certificate is pinned through the CA
    // file, but the connection is made to a resolved address whose IP may not
    // be a SAN of the certificate.
    if (!ca_path_.empty() && tls_->load_ca_file(ca_path_)) {
      tls_->set_verify_mode(uvcpp::tls_verify_mode::PEER);
    } else {
      tls_->set_verify_mode(uvcpp::tls_verify_mode::NONE);
    }
  }
  client_ = std::make_shared<uvcpp::uvcpp_tcp_client>(loop_);
  if (tls_ && client_->enable_tls(tls_.get()) != 0) {
    fail("relay TLS setup failed");
    return;
  }
  std::weak_ptr<relay_stream_transport> weak = weak_from_this();
  const int status = client_->connect(
      host_.c_str(), port_, [weak](int connect_status) {
        if (auto self = weak.lock()) self->on_connected(connect_status);
      });
  if (status != 0) fail("relay connect failed to start");
}

void relay_stream_transport::on_connected(int status) {
  if (closed_) return;
  if (status != 0) {
    fail("relay connect failed (" + std::to_string(status) + ")");
    return;
  }
  std::weak_ptr<relay_stream_transport> weak = weak_from_this();
  client_->read_start_events(
      [weak](uvcpp::uvcpp_tcp_client&, const uvcpp::net_read_result& result) {
        auto self = weak.lock();
        if (!self || self->closed_) return;
        if (result.event == uvcpp::net_read_event::DATA) {
          self->on_read(reinterpret_cast<const uint8_t*>(result.data),
                        result.size);
        } else {
          self->on_link_closed("relay connection closed");
        }
      });
  nlohmann::json handshake;
  handshake["connect_id"] = connect_id_;
  handshake["token"] = token_;
  std::string line = handshake.dump();
  line.push_back('\n');
  enqueue(std::move(line), nullptr);
}

void relay_stream_transport::on_read(const uint8_t* data, std::size_t len) {
  if (closed_) return;
  if (ready_) {
    if (inner_) inner_->feed(data, len);
    return;
  }
  handshake_.append(reinterpret_cast<const char*>(data), len);
  if (handshake_.size() > kMaxHandshakeBytes) {
    fail("relay handshake is too large");
    return;
  }
  const std::size_t newline = handshake_.find('\n');
  if (newline == std::string::npos) return;
  const std::string line = handshake_.substr(0, newline);
  const std::string rest = handshake_.substr(newline + 1);
  handshake_.clear();

  nlohmann::json reply;
  try {
    reply = nlohmann::json::parse(line);
  } catch (const std::exception&) {
    fail("relay handshake reply is not JSON");
    return;
  }
  if (!reply.is_object()) {
    fail("relay handshake reply is not an object");
    return;
  }
  const std::string type = reply.value("type", std::string());
  if (type == "relay-error") {
    fail(reply.value("message", std::string("relay refused the session")));
    return;
  }
  if (type != "relay-ready") {
    fail("unexpected relay handshake reply");
    return;
  }
  become_ready();
  if (!rest.empty() && inner_) {
    inner_->feed(reinterpret_cast<const uint8_t*>(rest.data()), rest.size());
  }
}

void relay_stream_transport::become_ready() {
  ready_ = true;
  std::weak_ptr<relay_stream_transport> weak = weak_from_this();
  auto link = std::make_shared<stream_transport>(
      [weak](const uint8_t* data, std::size_t len) {
        auto self = weak.lock();
        if (!self || self->closed_) return false;
        self->enqueue(std::string(reinterpret_cast<const char*>(data), len),
                      nullptr);
        return true;
      },
      [weak](int, const std::string& reason) {
        auto self = weak.lock();
        if (self) self->fail(reason.empty() ? "relay closed" : reason);
      },
      protocol_.c_str());
  inner_ = link;
  inner_->set_handlers(
      [weak](const std::string& text) {
        auto self = weak.lock();
        if (self && self->text_) self->text_(text);
      },
      [weak](const uint8_t* data, std::size_t len) {
        auto self = weak.lock();
        if (self && self->binary_) self->binary_(data, len);
      },
      [weak](int code, const std::string& reason) {
        auto self = weak.lock();
        if (self) self->deliver_close(code, reason);
      });
  if (ready_handler_) {
    std::function<void()> handler = ready_handler_;
    handler();
  }
}

bool relay_stream_transport::send_text(const std::string& text) {
  if (!ready_ || closed_ || !inner_) return false;
  return inner_->send_text(text);
}

bool relay_stream_transport::send_binary(const uint8_t* data, std::size_t len) {
  if (!ready_ || closed_ || !inner_) return false;
  return inner_->send_binary(data, len);
}

void relay_stream_transport::close(int code, const std::string& reason) {
  (void)code;
  (void)reason;
  if (closed_) return;
  closed_ = true;
  fail_pending();
  if (client_) client_->close();
}

void relay_stream_transport::deliver_close(int code,
                                           const std::string& reason) {
  if (close_delivered_) return;
  close_delivered_ = true;
  closed_ = true;
  fail_pending();
  if (on_close_) {
    close_handler handler = std::move(on_close_);
    on_close_ = nullptr;
    handler(code, reason);
  }
}

void relay_stream_transport::on_link_closed(const std::string& reason) {
  if (closed_ && close_delivered_) return;
  if (inner_) {
    inner_->peer_closed(reason);
  } else {
    deliver_close(transport_close::policy, reason);
  }
}

void relay_stream_transport::fail(const std::string& reason) {
  if (closed_ && close_delivered_) return;
  closed_ = true;
  if (client_) client_->close();
  if (inner_) {
    inner_->peer_closed(reason);
  } else {
    deliver_close(transport_close::policy, reason);
  }
}

void relay_stream_transport::enqueue(std::string bytes,
                                     std::function<void(int)> done) {
  if (closed_ || client_ == nullptr) {
    if (done) done(-1);
    return;
  }
  if (queued_ + bytes.size() > kMaxBacklogBytes) {
    if (done) done(-1);
    fail("relay send backlog exceeded");
    return;
  }
  queued_ += bytes.size();
  queue_.push_back({std::move(bytes), std::move(done)});
  pump();
}

void relay_stream_transport::pump() {
  if (writing_ || queue_.empty() || closed_ || client_ == nullptr) return;
  writing_ = true;
  outbound entry = std::move(queue_.front());
  queue_.pop_front();
  const std::size_t size = entry.bytes.size();
  auto bytes = std::make_shared<std::string>(std::move(entry.bytes));
  auto done = std::move(entry.done);
  std::weak_ptr<relay_stream_transport> weak = weak_from_this();
  const int status = client_->write(
      bytes->data(), bytes->size(),
      [weak, bytes, size, done](int write_status) {
        if (done) done(write_status);
        auto self = weak.lock();
        if (!self) return;
        self->writing_ = false;
        self->queued_ -= size;
        if (write_status != 0) {
          self->fail("relay write failed");
          return;
        }
        self->pump();
      });
  if (status != 0) {
    writing_ = false;
    queued_ -= size;
    if (done) done(status);
    fail("relay write failed");
  }
}

void relay_stream_transport::fail_pending() {
  std::deque<outbound> pending;
  pending.swap(queue_);
  queued_ = 0;
  for (outbound& entry : pending) {
    if (entry.done) entry.done(-1);
  }
}

}  // namespace molink
