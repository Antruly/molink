#include "transport/quic_relay_transport.h"

#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <uvcpp/uvcpp_config.h>

#if UVCPP_QUIC_ENABLE
#include <handle/uvcpp_loop.h>
#include <net/uvcpp_net_read.h>
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_connection.h>
#include <ssl/uvcpp_ssl_context.h>
#endif

#include "protocol/frame_packet.h"

namespace molink {
namespace {

constexpr std::size_t kMaxBacklogBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxHandshakeBytes = 4096;

}  // namespace

quic_relay_transport::quic_relay_transport(std::string host, int port,
                                           std::string connect_id,
                                           std::string token,
                                           std::string ca_path,
                                           uvcpp::uvcpp_loop* loop,
                                           post_fn post)
    : host_(std::move(host)),
      port_(port),
      connect_id_(std::move(connect_id)),
      token_(std::move(token)),
      ca_path_(std::move(ca_path)),
      loop_(loop),
      post_(std::move(post)) {}

quic_relay_transport::~quic_relay_transport() {
  closed_ = true;
#if UVCPP_QUIC_ENABLE
  if (client_ && client_->connection() != nullptr) {
    client_->connection()->close();
  }
#endif
}

bool quic_relay_transport::is_open() const { return ready_ && !closed_; }

void quic_relay_transport::set_ready_handler(std::function<void()> ready) {
  ready_handler_ = std::move(ready);
  if (ready_ && ready_handler_) {
    std::function<void()> handler = ready_handler_;
    handler();
  }
}

void quic_relay_transport::set_handlers(text_handler on_text,
                                        binary_handler on_binary,
                                        close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  on_close_ = std::move(on_close);
}

void quic_relay_transport::start() {
  if (started_) return;
  started_ = true;
#if !UVCPP_QUIC_ENABLE
  fail("this build has no QUIC");
#else
  tls_ = std::make_shared<uvcpp::uvcpp_ssl_context>(uvcpp::tls_mode::CLIENT);
  if (!ca_path_.empty() && tls_->load_ca_file(ca_path_)) {
    tls_->set_verify_mode(uvcpp::tls_verify_mode::PEER);
  } else {
    tls_->set_verify_mode(uvcpp::tls_verify_mode::NONE);
  }
  tls_->set_alpn_protos({"molink-relay"});
  client_ = std::make_shared<uvcpp::uvcpp_quic_client>(loop_);
  client_->set_ssl_context(tls_.get());
  client_->set_alpn_protos({"molink-relay"});
  std::weak_ptr<quic_relay_transport> weak = weak_from_this();
  const int status = client_->connect(
      host_.c_str(), port_, [weak](int connect_status) {
        if (auto self = weak.lock()) self->on_connected(connect_status);
      });
  if (status != 0) fail("relay QUIC connect failed to start");
#endif
}

#if UVCPP_QUIC_ENABLE
void quic_relay_transport::on_connected(int status) {
  if (closed_) return;
  uvcpp::uvcpp_quic_connection* connection =
      client_ ? client_->connection() : nullptr;
  if (status != 0 || connection == nullptr) {
    fail("relay QUIC handshake failed (" + std::to_string(status) + ")");
    return;
  }
  std::weak_ptr<quic_relay_transport> weak = weak_from_this();
  uvcpp::uvcpp_quic_connection::callbacks callbacks;
  callbacks.on_read = [weak](uvcpp::uvcpp_quic_connection&, int64_t,
                             const uvcpp::net_read_result& result) {
    auto self = weak.lock();
    if (!self || self->closed_) return;
    if (result.event == uvcpp::net_read_event::DATA) {
      self->on_read(reinterpret_cast<const uint8_t*>(result.data),
                    result.size);
    } else {
      self->on_link_closed("relay connection closed");
    }
  };
  callbacks.on_write = [weak](uvcpp::uvcpp_quic_connection&, int64_t,
                              int write_status) {
    auto self = weak.lock();
    if (self) self->on_write(write_status);
  };
  callbacks.on_close = [weak](uvcpp::uvcpp_quic_connection&, int) {
    auto self = weak.lock();
    if (self) self->on_link_closed("relay connection closed");
  };
  connection->set_callbacks(callbacks);

  const int64_t stream_id = connection->open_stream(true);
  if (stream_id < 0) {
    fail("relay QUIC open_stream failed");
    return;
  }
  stream_id_ = stream_id;
  nlohmann::json handshake;
  handshake["connect_id"] = connect_id_;
  handshake["token"] = token_;
  std::string line = handshake.dump();
  line.push_back('\n');
  write_bytes(reinterpret_cast<const uint8_t*>(line.data()), line.size(),
              nullptr);
}
#else
void quic_relay_transport::on_connected(int status) { (void)status; }
#endif

void quic_relay_transport::on_read(const uint8_t* data, std::size_t len) {
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

void quic_relay_transport::become_ready() {
  ready_ = true;
  std::weak_ptr<quic_relay_transport> weak = weak_from_this();
  auto link = std::make_shared<stream_transport>(
      [weak](const uint8_t* data, std::size_t len) {
        auto self = weak.lock();
        if (!self || self->closed_) return false;
        return self->write_bytes(data, len, nullptr);
      },
      [weak](int, const std::string& reason) {
        auto self = weak.lock();
        if (self) self->fail(reason.empty() ? "relay closed" : reason);
      },
      "quic");
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

bool quic_relay_transport::send_text(const std::string& text) {
  if (!ready_ || closed_ || !inner_) return false;
  return inner_->send_text(text);
}

bool quic_relay_transport::send_binary(const uint8_t* data, std::size_t len) {
  if (!ready_ || closed_ || !inner_) return false;
  return inner_->send_binary(data, len);
}

bool quic_relay_transport::write_bytes(const uint8_t* data, std::size_t len,
                                       std::function<void(int)> done) {
#if !UVCPP_QUIC_ENABLE
  (void)data;
  (void)len;
  if (done) done(-1);
  return false;
#else
  uvcpp::uvcpp_quic_connection* connection =
      client_ ? client_->connection() : nullptr;
  if (closed_ || connection == nullptr) {
    if (done) done(-1);
    return false;
  }
  if (queued_ + len > kMaxBacklogBytes) {
    if (done) done(-1);
    fail("relay send backlog exceeded");
    return false;
  }
  const int status = connection->write_stream(
      stream_id_, reinterpret_cast<const char*>(data), len, false);
  if (status != 0) {
    if (done) done(status);
    fail("relay write failed");
    return false;
  }
  queued_ += len;
  writes_.push_back({len, std::move(done)});
  return true;
#endif
}

void quic_relay_transport::on_write(int status) {
  if (writes_.empty()) return;
  pending_write write = std::move(writes_.front());
  writes_.pop_front();
  queued_ -= write.size;
  if (write.done) write.done(status);
  if (status != 0) fail("relay write failed");
}

void quic_relay_transport::close(int code, const std::string& reason) {
  (void)code;
  (void)reason;
  if (closed_) return;
  closed_ = true;
  fail_pending();
#if UVCPP_QUIC_ENABLE
  if (client_ && client_->connection() != nullptr) {
    client_->connection()->close();
  }
#endif
}

void quic_relay_transport::deliver_close(int code,
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

void quic_relay_transport::on_link_closed(const std::string& reason) {
  if (closed_ && close_delivered_) return;
  closed_ = true;
  fail_pending();
  deliver_close(transport_close::policy, reason);
}

void quic_relay_transport::fail(const std::string& reason) {
  if (closed_ && close_delivered_) return;
  closed_ = true;
  fail_pending();
#if UVCPP_QUIC_ENABLE
  if (client_ && client_->connection() != nullptr) {
    client_->connection()->close();
  }
#endif
  deliver_close(transport_close::policy, reason);
}

void quic_relay_transport::fail_pending() {
  std::deque<pending_write> writes;
  writes.swap(writes_);
  queued_ = 0;
  for (pending_write& write : writes) {
    if (write.done) write.done(-1);
  }
}

}  // namespace molink
