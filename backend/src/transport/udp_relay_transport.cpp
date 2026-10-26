#include "transport/udp_relay_transport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <uv.h>

#include <handle/uvcpp_loop.h>
#include <handle/uvcpp_timer.h>
#include <handle/uvcpp_udp.h>
#include <uvcpp/uvcpp_buf.h>
#include <uvcpp/uvcpp_define.h>

#include "util.h"

namespace molink {
namespace {

using nlohmann::json;

constexpr uint8_t kTextTag = 0x00;
constexpr uint8_t kBinaryTag = 0x01;
constexpr uint8_t kFragmentTag = 0x02;
constexpr std::size_t kMaxDatagram = 1200;
constexpr std::size_t kFragmentHeader = 10;
constexpr std::size_t kFragmentChunk = kMaxDatagram - kFragmentHeader;
constexpr std::size_t kMaxPendingMessages = 16;
constexpr std::size_t kMaxQueuedBytes = 8u * 1024u * 1024u;
constexpr uint64_t kFragmentTtlMs = 5000;
constexpr uint64_t kHandshakeRetryMs = 500;
constexpr std::size_t kPacerBatch = 24;
constexpr std::size_t kPacerWatermark = 256u * 1024u;
constexpr uint64_t kPacerIntervalMs = 2;

uint32_t read_u32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t read_u16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

void write_u32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

void write_u16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

bool same_endpoint(const struct sockaddr* a, const struct sockaddr* b) {
  if (a == nullptr || b == nullptr || a->sa_family != b->sa_family) {
    return false;
  }
  if (a->sa_family == AF_INET) {
    auto* va = reinterpret_cast<const struct sockaddr_in*>(a);
    auto* vb = reinterpret_cast<const struct sockaddr_in*>(b);
    return va->sin_addr.s_addr == vb->sin_addr.s_addr &&
           va->sin_port == vb->sin_port;
  }
  if (a->sa_family == AF_INET6) {
    auto* va = reinterpret_cast<const struct sockaddr_in6*>(a);
    auto* vb = reinterpret_cast<const struct sockaddr_in6*>(b);
    return std::memcmp(&va->sin6_addr, &vb->sin6_addr,
                       sizeof(va->sin6_addr)) == 0 &&
           va->sin6_port == vb->sin6_port;
  }
  return false;
}

}  // namespace

udp_relay_transport::udp_relay_transport(std::string host, int port,
                                         std::string connect_id,
                                         std::string token,
                                         uvcpp::uvcpp_loop* loop,
                                         post_fn post)
    : host_(std::move(host)),
      port_(port),
      connect_id_(std::move(connect_id)),
      token_(std::move(token)),
      loop_(loop),
      post_(std::move(post)) {}

udp_relay_transport::~udp_relay_transport() {
  closed_ = true;
  if (retry_) retry_->stop();
  if (pacer_) pacer_->stop();
  pending_.clear();
  udp_.reset();
}

bool udp_relay_transport::is_open() const { return ready_ && !closed_; }

void udp_relay_transport::set_ready_handler(std::function<void()> ready) {
  ready_handler_ = std::move(ready);
  if (ready_ && !closed_ && ready_handler_) {
    std::function<void()> handler = ready_handler_;
    handler();
  }
}

void udp_relay_transport::set_handlers(text_handler on_text,
                                       binary_handler on_binary,
                                       close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  on_close_ = std::move(on_close);
}

void udp_relay_transport::start() {
  if (started_) return;
  started_ = true;
  if (loop_ == nullptr) {
    fail("relay UDP: no loop");
    return;
  }
  auto address = std::make_unique<struct sockaddr_storage>();
  std::memset(address.get(), 0, sizeof(*address));
  auto* v4 = reinterpret_cast<struct sockaddr_in*>(address.get());
  if (uv_ip4_addr(host_.c_str(), port_, v4) != 0) {
    fail("relay UDP: invalid server address");
    return;
  }
  server_address_ = std::move(address);
  server_address_len_ = sizeof(struct sockaddr_in);

  udp_ = std::make_unique<uvcpp::uvcpp_udp>(loop_);
  struct sockaddr_in bind_address;
  std::memset(&bind_address, 0, sizeof(bind_address));
  bind_address.sin_family = AF_INET;
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_address.sin_port = 0;
  if (udp_->bind(reinterpret_cast<const struct sockaddr*>(&bind_address), 0) !=
      0) {
    fail("relay UDP: cannot bind a local port");
    return;
  }
  // Large keyframes arrive as hundreds of back-to-back datagrams; the default
  // socket buffer drops the tail on a fast path. Ask for room to land a frame.
  int buffer_size = 4 * 1024 * 1024;
  uv_recv_buffer_size(udp_->get_handle(), &buffer_size);
  buffer_size = 4 * 1024 * 1024;
  uv_send_buffer_size(udp_->get_handle(), &buffer_size);

  std::weak_ptr<udp_relay_transport> weak = weak_from_this();
  if (udp_->recv_start(
          [](uvcpp::uvcpp_handle*, std::size_t size, uv_buf_t* buf) {
            uvcpp::uvcpp_buf::alloc_buf(buf, size > 0 ? size : 2048);
          },
          [weak](uvcpp::uvcpp_udp*, ssize_t nread, const uv_buf_t* buf,
                 const struct sockaddr* addr, unsigned int) {
            if (nread > 0 && buf->base != nullptr && addr != nullptr) {
              if (auto self = weak.lock()) {
                if (self->server_address_ != nullptr &&
                    same_endpoint(
                        addr,
                        reinterpret_cast<const struct sockaddr*>(
                            self->server_address_.get()))) {
                  self->on_datagram(
                      reinterpret_cast<const uint8_t*>(buf->base),
                      static_cast<std::size_t>(nread));
                }
              }
            }
            if (buf->base != nullptr) uvcpp::uvcpp_free_bytes(buf->base);
          }) != 0) {
    fail("relay UDP: cannot receive");
    return;
  }

  pacer_ = std::make_unique<uvcpp::uvcpp_timer>(loop_);
  pacer_->init();
  retry_ = std::make_unique<uvcpp::uvcpp_timer>(loop_);
  retry_->init();
  retry_->start(
      [weak](uvcpp::uvcpp_timer*) {
        if (auto self = weak.lock()) self->send_handshake();
      },
      kHandshakeRetryMs, kHandshakeRetryMs);
  send_handshake();
}

void udp_relay_transport::send_handshake() {
  if (closed_ || ready_ || udp_ == nullptr) return;
  json body;
  body["connect_id"] = connect_id_;
  body["token"] = token_;
  const std::string text = body.dump();
  send_datagram(reinterpret_cast<const uint8_t*>(text.data()), text.size(),
                nullptr);
}

void udp_relay_transport::send_datagram(const uint8_t* data, std::size_t len,
                                        std::function<void(int)> done) {
  if (closed_ || udp_ == nullptr || server_address_ == nullptr) {
    if (done) done(-1);
    return;
  }
  struct send_state {
    std::vector<uint8_t> bytes;
    uv_buf_t buffer{};
    uvcpp::uvcpp_udp_send* request = nullptr;
    std::function<void(int)> done;
  };
  auto* buffer = new send_state();
  buffer->bytes.assign(data, data + len);
  buffer->buffer.base = reinterpret_cast<char*>(buffer->bytes.data());
  buffer->buffer.len = buffer->bytes.size();
  buffer->done = std::move(done);
  auto* request = new uvcpp::uvcpp_udp_send();
  request->init();
  request->set_uv_buf(&buffer->buffer, false);
  request->set_data(buffer);
  buffer->request = request;
  const int rc = udp_->send(
      request, &buffer->buffer, 1,
      reinterpret_cast<const struct sockaddr*>(server_address_.get()),
      [](uvcpp::uvcpp_udp_send* req, int status) {
        auto* state = static_cast<send_state*>(req->get_data());
        if (state != nullptr) {
          if (state->done) state->done(status);
          delete state;
        }
        delete req;
      });
  if (rc != 0) {
    if (buffer->done) buffer->done(rc);
    delete buffer;
    delete request;
  }
}

void udp_relay_transport::on_datagram(const uint8_t* data, std::size_t len) {
  if (closed_ || data == nullptr || len == 0) return;
  if (!ready_) {
    // Before pairing the server only sends JSON notices.
    if (data[0] == '{') {
      handle_notice(std::string(reinterpret_cast<const char*>(data), len));
    }
    return;
  }
  if (data[0] == '{') {
    handle_notice(std::string(reinterpret_cast<const char*>(data), len));
    return;
  }
  handle_data(data, len);
}

void udp_relay_transport::handle_notice(const std::string& text) {
  json body;
  try {
    body = json::parse(text);
  } catch (const std::exception&) {
    return;
  }
  if (!body.is_object()) return;
  const std::string type = body.value("type", std::string());
  if (type == "relay-ready") {
    become_ready();
    return;
  }
  if (type == "relay-error") {
    fail(body.value("message", std::string("relay refused the session")));
    return;
  }
  if (type == "relay-closed") {
    fail("relay closed");
  }
}

void udp_relay_transport::become_ready() {
  if (ready_ || closed_) return;
  ready_ = true;
  if (retry_) retry_->stop();
  if (ready_handler_) {
    std::function<void()> handler = ready_handler_;
    handler();
  }
}

void udp_relay_transport::handle_data(const uint8_t* data, std::size_t len) {
  const uint8_t tag = data[0];
  if (tag == kTextTag || tag == kBinaryTag) {
    const uint8_t* payload = data + 1;
    const std::size_t size = len - 1;
    if (tag == kTextTag) {
      if (text_) {
        text_(std::string(reinterpret_cast<const char*>(payload), size));
      }
    } else if (binary_) {
      binary_(payload, size);
    }
    return;
  }
  if (tag != kFragmentTag || len < kFragmentHeader) return;

  const uint8_t kind = data[1];
  const uint32_t id = read_u32(data + 2);
  const uint16_t index = read_u16(data + 6);
  const uint16_t count = read_u16(data + 8);
  if (count == 0 || index >= count) return;
  const uint8_t* payload = data + kFragmentHeader;
  const std::size_t size = len - kFragmentHeader;
  const uint64_t now = now_ms();

  auto it = fragments_.find(id);
  if (it == fragments_.end()) {
    if (std::getenv("MOLINK_UDP_LOG") != nullptr && count > 1) {
      std::fprintf(stderr, "udp relay: fragment set %u of %u\n", id,
                   static_cast<unsigned>(count));
    }
    // Expire dead partial messages before adding another.
    for (auto old = fragments_.begin(); old != fragments_.end();) {
      if (now > old->second.updated_ms &&
          now - old->second.updated_ms > kFragmentTtlMs) {
        old = fragments_.erase(old);
      } else {
        ++old;
      }
    }
    if (fragments_.size() >= kMaxPendingMessages) {
      fragments_.erase(fragments_.begin());
    }
    fragment_set entry;
    entry.kind = kind;
    entry.count = count;
    entry.updated_ms = now;
    entry.parts.resize(count);
    entry.have.assign(count, false);
    it = fragments_.emplace(id, std::move(entry)).first;
  }
  fragment_set& entry = it->second;
  if (entry.count != count || entry.kind != kind) return;
  if (index >= entry.parts.size()) return;
  entry.updated_ms = now;
  if (!entry.have[index]) {
    entry.parts[index].assign(reinterpret_cast<const char*>(payload), size);
    entry.have[index] = true;
    ++entry.received;
  }
  if (entry.received < entry.count) return;

  std::string message;
  for (const std::string& part : entry.parts) message += part;
  const uint8_t message_kind = entry.kind;
  fragments_.erase(it);
  if (std::getenv("MOLINK_UDP_LOG") != nullptr) {
    std::fprintf(stderr, "udp relay: reassembled %zu bytes\n", message.size());
  }
  if (message_kind == kTextTag) {
    if (text_) text_(message);
  } else if (binary_) {
    binary_(reinterpret_cast<const uint8_t*>(message.data()), message.size());
  }
}

bool udp_relay_transport::send_message(uint8_t kind, const uint8_t* data,
                                       std::size_t len) {
  if (!ready_ || closed_ || udp_ == nullptr ||
      (data == nullptr && len > 0)) {
    return false;
  }
  const uint8_t message_kind = kind == kBinaryTag ? kBinaryTag : kTextTag;
  if (len + 1 <= kMaxDatagram) {
    std::string datagram;
    datagram.reserve(len + 1);
    datagram.push_back(static_cast<char>(message_kind));
    if (len > 0) {
      datagram.append(reinterpret_cast<const char*>(data), len);
    }
    enqueue_datagram(std::move(datagram));
    return true;
  }

  const std::size_t count = (len + kFragmentChunk - 1) / kFragmentChunk;
  if (count == 0 || count > 0xFFFFu) return false;
  const bool trace = std::getenv("MOLINK_UDP_LOG") != nullptr;
  if (trace) {
    std::fprintf(stderr, "udp relay: send %zu bytes as %zu fragments\n", len,
                 count);
  }
  const uint32_t id = next_message_id_++;
  bool ok = true;
  for (std::size_t index = 0; index < count; ++index) {
    // Lossy path: once the loop is far behind, drop the rest instead of
    // queueing megabytes; the receiver's tile-clock repair asks for it.
    if (udp_->get_send_queue_size() > kMaxQueuedBytes) return false;
    const std::size_t offset = index * kFragmentChunk;
    const std::size_t remaining = len - offset;
    const std::size_t size =
        remaining < kFragmentChunk ? remaining : kFragmentChunk;
    std::string datagram(kFragmentHeader + size, '\0');
    auto* bytes = reinterpret_cast<uint8_t*>(&datagram[0]);
    bytes[0] = kFragmentTag;
    bytes[1] = message_kind;
    write_u32(bytes + 2, id);
    write_u16(bytes + 6, static_cast<uint16_t>(index));
    write_u16(bytes + 8, static_cast<uint16_t>(count));
    std::memcpy(bytes + kFragmentHeader, data + offset, size);
    enqueue_datagram(std::move(datagram));
    ok = true;
  }
  return ok;
}

void udp_relay_transport::enqueue_datagram(std::string bytes) {
  if (closed_ || udp_ == nullptr) return;
  if (pending_bytes_ + bytes.size() > kMaxQueuedBytes) {
    // Far behind: drop this datagram rather than grow without bound. The
    // receiver's tile-clock repair asks for what it missed.
    return;
  }
  pending_bytes_ += bytes.size();
  pending_.push_back(std::move(bytes));
  pump_datagrams();
}

void udp_relay_transport::pump_datagrams() {
  if (closed_ || udp_ == nullptr) {
    pending_.clear();
    pending_bytes_ = 0;
    return;
  }
  std::size_t sent = 0;
  while (!pending_.empty() && sent < kPacerBatch &&
         udp_->get_send_queue_size() < kPacerWatermark) {
    std::string datagram = std::move(pending_.front());
    pending_.pop_front();
    pending_bytes_ -= datagram.size();
    send_datagram(reinterpret_cast<const uint8_t*>(datagram.data()),
                  datagram.size(), nullptr);
    ++sent;
  }
  if (pending_.empty()) {
    if (pacer_ && pacer_running_) {
      pacer_->stop();
      pacer_running_ = false;
    }
    return;
  }
  if (pacer_ && !pacer_running_) {
    std::weak_ptr<udp_relay_transport> weak = weak_from_this();
    pacer_->start(
        [weak](uvcpp::uvcpp_timer*) {
          if (auto self = weak.lock()) self->pump_datagrams();
        },
        kPacerIntervalMs, kPacerIntervalMs);
    pacer_running_ = true;
  }
}

bool udp_relay_transport::send_text(const std::string& text) {
  return send_message(kTextTag, reinterpret_cast<const uint8_t*>(text.data()),
                      text.size());
}

bool udp_relay_transport::send_binary(const uint8_t* data, std::size_t len) {
  return send_message(kBinaryTag, data, len);
}

void udp_relay_transport::close(int code, const std::string& reason) {
  if (closed_) return;
  if (retry_) retry_->stop();
  // Best effort: tell the other side the relay is gone. The server still
  // forwards datagrams from an attached peer, so a JSON notice reaches it.
  // This has to run before closed_ is set: send_datagram refuses when closed.
  if (ready_ && udp_ != nullptr) {
    const std::string notice = "{\"type\":\"relay-closed\"}";
    send_datagram(reinterpret_cast<const uint8_t*>(notice.data()),
                  notice.size(), nullptr);
  }
  closed_ = true;
  pending_.clear();
  pending_bytes_ = 0;
  if (pacer_) pacer_->stop();
  pacer_running_ = false;
  (void)code;
  (void)reason;
}

void udp_relay_transport::deliver_close(int code, const std::string& reason) {
  if (close_delivered_) return;
  close_delivered_ = true;
  closed_ = true;
  if (retry_) retry_->stop();
  if (on_close_) {
    close_handler handler = std::move(on_close_);
    on_close_ = nullptr;
    handler(code, reason);
  }
}

void udp_relay_transport::fail(const std::string& reason) {
  if (closed_ && close_delivered_) return;
  closed_ = true;
  if (retry_) retry_->stop();
  deliver_close(transport_close::policy, reason);
}

}  // namespace molink
