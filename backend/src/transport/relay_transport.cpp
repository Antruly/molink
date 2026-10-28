#include "transport/relay_transport.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

#include <uvcpp/uvcpp_config.h>
#include <webapp/uvcpp_web_ws_client.h>
#if UVCPP_OPENSSL_ENABLE
#include <ssl/uvcpp_ssl_context.h>
#endif

#include "protocol/frame_packet.h"

namespace molink {
namespace {

/// The untyped send path still carries whole sessions: the browser proxies
/// forward page traffic through it. Classify by packet header so those bytes
/// land in the channel whose budget and drain order they belong to instead of
/// piling into the small control queue.
transport_channel channel_for(const uint8_t* data, std::size_t len) {
  switch (protocol::packet_stream_of(data, len)) {
    case protocol::packet_stream::video:
      return transport_channel::video;
    case protocol::packet_stream::audio:
      return transport_channel::audio;
    case protocol::packet_stream::file:
      return transport_channel::file;
    case protocol::packet_stream::control:
      break;
  }
  return transport_channel::control;
}

}  // namespace

relay_transport::relay_transport(std::string url, std::string ca_path,
                                 post_fn post, bool relay_protocol)
    : url_(std::move(url)), ca_path_(std::move(ca_path)), post_(std::move(post)),
      relay_protocol_(relay_protocol) {}

relay_transport::~relay_transport() { close(transport_close::normal, "gone"); }

void relay_transport::set_handlers(text_handler on_text,
                                   binary_handler on_binary,
                                   close_handler on_close) {
  std::lock_guard<std::mutex> lock(handler_mutex_);
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  close_ = std::move(on_close);
}

void relay_transport::set_ready_handler(std::function<void()> ready) {
  std::function<void()> notify;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    ready_handler_ = std::move(ready);
    if (ready_) notify = ready_handler_;
  }
  if (notify) {
    if (post_) {
      post_(std::move(notify));
    } else {
      notify();
    }
  }
}

void relay_transport::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { thread_main(); });
}

bool relay_transport::is_open() const {
  return ready_.load() && !closed_.load();
}

bool relay_transport::send_text(const std::string& text) {
  return enqueue(transport_channel::control, true, text);
}

bool relay_transport::send_text(transport_channel channel,
                                const std::string& text) {
  if (channel != transport_channel::control) return false;
  return enqueue(channel, true, text);
}

bool relay_transport::send_binary(const uint8_t* data, std::size_t len) {
  return send_binary(channel_for(data, len), data, len);
}

bool relay_transport::send_binary(transport_channel channel,
                                  const uint8_t* data, std::size_t len) {
  if (data == nullptr && len > 0) return false;
  // A keyframe is the only queued frame whose loss costs a full refresh, so
  // tell the scheduler which video messages must not be evicted.
  const bool keyframe =
      channel == transport_channel::video &&
      protocol::packet_is_keyframe(data, len);
  std::string copy(reinterpret_cast<const char*>(data), len);
  return enqueue(channel, false, std::move(copy), keyframe);
}

bool relay_transport::enqueue(transport_channel channel, bool text,
                              std::string data, bool keyframe) {
  if (!ready_.load() || closed_.load()) return false;
  std::lock_guard<std::mutex> lock(send_mutex_);
  if (text) {
    return channel == transport_channel::control && sends_.enqueue_text(data);
  }
  return sends_.enqueue(channel,
                        reinterpret_cast<const uint8_t*>(data.data()),
                        data.size(), keyframe);
}

void relay_transport::deliver_close(int code, const std::string& reason) {
  if (close_delivered_.exchange(true)) return;
  close_handler handler;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler = close_;
  }
  if (!handler) return;
  if (post_) {
    post_([handler, code, reason] { handler(code, reason); });
  } else {
    handler(code, reason);
  }
}

void relay_transport::close(int code, const std::string& reason) {
  (void)code;
  (void)reason;
  closed_ = true;
  running_ = false;
  if (thread_.joinable()) {
    if (std::this_thread::get_id() == thread_.get_id()) return;
    thread_.join();
  }
}

void relay_transport::thread_main() {
#if UVCPP_OPENSSL_ENABLE
  // Must outlive `client`: the WebSocket keeps a pointer to this context and
  // uses it from its destructor.
  uvcpp::uvcpp_ssl_context tls(uvcpp::tls_mode::CLIENT);
#endif
  uvcpp::uvcpp_web_ws_client client;

  std::fprintf(stderr, "relay: connecting\n");
  std::fflush(stderr);
  if (url_.rfind("wss://", 0) == 0) {
#if UVCPP_OPENSSL_ENABLE
    tls.set_verify_mode(uvcpp::tls_verify_mode::PEER_STRICT);
    if (!ca_path_.empty() && !tls.load_ca_file(ca_path_)) {
      std::fprintf(stderr, "relay: cannot load CA %s\n", ca_path_.c_str());
      std::fflush(stderr);
      closed_ = true;
      deliver_close(transport_close::policy, "cannot load the relay CA");
      running_ = false;
      return;
    }
    client.set_ssl_context(&tls);
#else
    std::fprintf(stderr, "relay: this build has no TLS; a wss:// relay is unavailable\n");
    std::fflush(stderr);
    closed_ = true;
    running_ = false;
    deliver_close(transport_close::policy, "this build has no TLS");
    return;
#endif
  }

  client.on_text([this](const std::string& text) {
    if (relay_protocol_ &&
        text.find("\"relay-ready\"") != std::string::npos) {
      ready_ = true;
      std::fprintf(stderr, "relay: ready\n");
      std::fflush(stderr);
      std::function<void()> handler;
      {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = ready_handler_;
      }
      if (handler) {
        if (post_) {
          post_(std::move(handler));
        } else {
          handler();
        }
      }
      return;
    }
    if (relay_protocol_ &&
        text.find("\"relay-closed\"") != std::string::npos) {
      closed_ = true;
      ready_ = false;
      deliver_close(transport_close::normal, "relay closed");
      stop_requested_ = true;
      return;
    }
    text_handler handler;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      handler = text_;
    }
    if (handler) {
      if (post_) {
        post_([handler, text] { handler(text); });
      } else {
        handler(text);
      }
    }
  });

  client.on_binary([this](const uint8_t* data, std::size_t len) {
    std::string copy(reinterpret_cast<const char*>(data), len);
    binary_handler handler;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      handler = binary_;
    }
    if (handler) {
      if (post_) {
        post_([handler, copy] {
          handler(reinterpret_cast<const uint8_t*>(copy.data()), copy.size());
        });
      } else {
        handler(reinterpret_cast<const uint8_t*>(copy.data()), copy.size());
      }
    }
  });

  client.on_close([this](uvcpp::ws_close_code, const std::string& reason) {
    std::fprintf(stderr, "relay: closed %s\n", reason.c_str());
    std::fflush(stderr);
    closed_ = true;
    ready_ = false;
    deliver_close(transport_close::normal, reason);
    stop_requested_ = true;
  });

  const int connect_rc = client.connect(url_);
  if (connect_rc != 0) {
    std::fprintf(stderr, "relay: connect rc=%d\n", connect_rc);
    std::fflush(stderr);
    closed_ = true;
    deliver_close(transport_close::policy, "cannot reach the relay");
    running_ = false;
    return;
  }

  if (!relay_protocol_) {
    // A plain session socket is usable as soon as the WebSocket handshake
    // completed; there is no pairing message to wait for.
    ready_ = true;
    std::function<void()> handler;
    {
      std::lock_guard<std::mutex> lock(handler_mutex_);
      handler = ready_handler_;
    }
    if (handler) {
      if (post_) {
        post_(std::move(handler));
      } else {
        handler();
      }
    }
  }

  while (running_) {
    client.run(UV_RUN_NOWAIT);
    {
      std::lock_guard<std::mutex> lock(send_mutex_);
      sends_.drain(
          [&client, this](const channel_scheduler::message& message) {
            if (client.session() == nullptr) return false;
            const std::size_t size = message.data.size();
            // libuvcpp's WebSocket client queues frames without a bound and
            // always returns 0, so a slow receiver would just grow its send
            // queue. Count the bytes until the write completes and stop
            // handing over messages past the cap: that is what lets
            // backpressure reach the scheduler and the file transfer offset.
            if (inflight_bytes_.load(std::memory_order_relaxed) + size >
                kMaxInflightBytes) {
              return false;
            }
            auto settle = [this, size](int) {
              inflight_bytes_.fetch_sub(size, std::memory_order_relaxed);
            };
            inflight_bytes_.fetch_add(size, std::memory_order_relaxed);
            int rc = 0;
            if (message.text) {
              rc = client.send_text(
                  std::string(message.data.begin(), message.data.end()),
                  settle);
            } else {
              rc = client.send_binary(
                  reinterpret_cast<const char*>(message.data.data()), size,
                  settle);
            }
            // The completion callback runs even when the send is refused
            // (queued-frame accounting settles it immediately), so it is the
            // single place that releases the bytes.
            return rc == 0;
          },
          64);
    }
    if (stop_requested_ || !running_) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  client.close();
}

}  // namespace molink
