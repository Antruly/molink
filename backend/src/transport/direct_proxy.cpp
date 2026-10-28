#include "transport/direct_proxy.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <net/uvcpp_net_read.h>
#include <net/uvcpp_tcp_client.h>
#include <ssl/uvcpp_ssl_context.h>
#include <uvcpp/uvcpp_config.h>

#ifndef MOLINK_HAVE_H2_WS
#define MOLINK_HAVE_H2_WS 0
#endif

#if UVCPP_QUIC_ENABLE || MOLINK_HAVE_H2_WS
#include <handle/uvcpp_loop.h>
#endif
#if UVCPP_QUIC_ENABLE
#include <quic/uvcpp_quic_client.h>
#include <quic/uvcpp_quic_connection.h>
#endif
#if MOLINK_HAVE_H2_WS
#include <handle/uvcpp_timer.h>
#include <http2/uvcpp_h2_connection.h>
#include <http2/uvcpp_h2_ws_connection.h>
#endif

#include "capture/av1_frame.h"
#include "compression.h"
#include "protocol/frame_packet.h"
#include "transport/h2_ws_transport.h"
#include "transport/peer_transports.h"
#include "transport/relay_transport.h"
#include "transport/tls_pin.h"
#include "transport/stream_transport.h"
#include "util.h"

namespace molink {
namespace {

/// Serializes writes onto the TCP client (one async write at a time) with a
/// bounded backlog, matching the host's tcp_listener.
constexpr std::size_t kMaxTcpBacklogBytes = 8u * 1024u * 1024u;

struct tcp_link_state {
  uvcpp::uvcpp_tcp_client* client = nullptr;
  std::shared_ptr<stream_transport> link;
  std::deque<std::string> queue;
  std::size_t queued = 0;
  bool writing = false;
  bool closed = false;
};

void tcp_link_fail(const std::shared_ptr<tcp_link_state>& state,
                   const std::string& reason) {
  if (state->closed) return;
  state->closed = true;
  state->queue.clear();
  state->queued = 0;
  uvcpp::uvcpp_tcp_client* client = state->client;
  state->client = nullptr;
  if (client != nullptr) {
    // Defer the session callback until the handle is closed: calling it from
    // inside a write completion would destroy the controller mid-call.
    client->close([state, reason] {
      if (state->link) state->link->peer_closed(reason);
    });
  }
}

void tcp_link_pump(const std::shared_ptr<tcp_link_state>& state) {
  if (state->writing || state->queue.empty() || state->closed) return;
  if (state->client == nullptr) {
    state->queue.clear();
    state->queued = 0;
    return;
  }
  auto chunk = std::make_shared<std::string>(std::move(state->queue.front()));
  state->queue.pop_front();
  const std::size_t size = chunk->size();
  state->writing = true;
  const int status = state->client->write(
      chunk->data(), chunk->size(), [state, chunk, size](int write_status) {
        state->writing = false;
        state->queued -= size;
        if (write_status != 0) {
          tcp_link_fail(state, "TCP write failed");
          return;
        }
        tcp_link_pump(state);
      });
  if (status != 0) {
    state->writing = false;
    state->queued -= size;
    tcp_link_fail(state, "TCP write failed");
  }
}

class direct_proxy final : public std::enable_shared_from_this<direct_proxy> {
 public:
  direct_proxy(uvcpp::uvcpp_ws_connection* browser, std::string host, int port,
               uvcpp::uvcpp_loop* loop,
               std::function<void(std::function<void()>)> post)
      : browser_(browser),
        host_(std::move(host)),
        port_(port),
        loop_(loop),
        post_(std::move(post)) {}

  ~direct_proxy() { stop_worker(); }

  void start() {
    if (browser_ == nullptr) return;
    browser_->set_max_message_size(1024u * 1024u);
    std::shared_ptr<direct_proxy> self = shared_from_this();
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

    start_worker();
    start_discovery();
  }

 private:
  /// The upstream leg has two shapes and the choice is made by the host:
  /// whisper to its `/api/transports` on a worker thread (the probe blocks),
  /// then either open one QUIC stream or keep the WebSocket fallback.
  void start_discovery() {
    if (post_ == nullptr) {
      start_ws_upstream();
      return;
    }
    const char* quic_setting = std::getenv("MOLINK_QUIC");
    if (quic_setting != nullptr &&
        (std::strcmp(quic_setting, "0") == 0 ||
         std::strcmp(quic_setting, "off") == 0)) {
      start_ws_upstream();
      return;
    }
#if !UVCPP_QUIC_ENABLE
    start_ws_upstream();
    return;
#else
    std::weak_ptr<direct_proxy> weak = weak_from_this();
    auto post = post_;
    // Detached on purpose: the probe can block for a few seconds, and the
    // browser may already be gone. It only touches copies and the weak_ptr,
    // and app.post drops the callback after shutdown.
    std::thread([weak, post, host = host_, port = port_] {
      const peer_transports info = probe_peer_transports(host, port);
      if (!info.reachable) {
        std::fprintf(stderr, "direct proxy: transport probe failed: %s\n",
                     info.error.c_str());
      }
      post([weak, info] {
        if (auto self = weak.lock()) self->on_peer_transports(info);
      });
    }).detach();
#endif
  }

  void on_peer_transports(const peer_transports& info) {
    if (closed_) return;
    tcp_port_ = info.tcp_port;
    http2_available_ = info.http2;
    if (!info.tls_cert.empty()) {
      pinned_certificate pin =
          pin_certificate(info.tls_cert, info.tls_fingerprint);
      if (pin.ok()) {
        pinned_ca_ = pin.ca_path;
        std::fprintf(stderr, "direct proxy: pinned peer certificate %s\n",
                     pin.fingerprint.c_str());
      } else {
        // A certificate that does not match its fingerprint is not a peer we
        // should pin; do not silently fall back to an unverified channel.
        std::fprintf(stderr, "direct proxy: %s; using the WebSocket fallback\n",
                     pin.error.c_str());
        start_ws_upstream();
        return;
      }
    }
#if UVCPP_QUIC_ENABLE
    if (info.quic_port > 0) {
      start_quic_upstream(info.quic_port);
      return;
    }
#endif
    if (info.tcp_port > 0) {
      start_tcp_upstream(info.tcp_port);
      return;
    }
    fallback_from_tcp();
  }

  /// The ladder: QUIC, then TCP, then HTTP/2 WebSocket, then the WebSocket
  /// baseline. h2c runs on the same web port the probe used.
  void fallback_from_quic() {
    if (tcp_port_ > 0) {
      start_tcp_upstream(tcp_port_);
      return;
    }
    fallback_from_tcp();
  }

  void fallback_from_tcp() {
#if MOLINK_HAVE_H2_WS
    if (http2_available_ && start_h2_upstream()) return;
#endif
    start_ws_upstream();
  }

  void start_tcp_upstream(int tcp_port) {
    if (closed_ || transport_ || loop_ == nullptr) {
      start_ws_upstream();
      return;
    }
    tcp_state_ = std::make_shared<tcp_link_state>();
    tcp_client_ = std::make_unique<uvcpp::uvcpp_tcp_client>(loop_);
    tcp_state_->client = tcp_client_.get();
#if UVCPP_OPENSSL_ENABLE
    // The host's TCP data listener is TLS-only. Pin it to the certificate the
    // probe advertised; no advertised certificate means an older host, which
    // keeps the pre-pinning behaviour.
    tcp_tls_ = std::make_shared<uvcpp::uvcpp_ssl_context>(
        uvcpp::tls_mode::CLIENT);
    if (!pinned_ca_.empty() && tcp_tls_->load_ca_file(pinned_ca_)) {
      tcp_tls_->set_verify_mode(uvcpp::tls_verify_mode::PEER);
    } else {
      tcp_tls_->set_verify_mode(uvcpp::tls_verify_mode::NONE);
    }
    if (tcp_client_->enable_tls(tcp_tls_.get()) != 0) {
      std::fprintf(stderr, "direct proxy: TCP TLS setup failed\n");
      tcp_client_.reset();
      tcp_state_.reset();
      fallback_from_tcp();
      return;
    }
#else
    std::fprintf(stderr, "direct proxy: this build has no TLS\n");
    tcp_client_.reset();
    tcp_state_.reset();
    fallback_from_tcp();
    return;
#endif
    std::weak_ptr<direct_proxy> weak = weak_from_this();
    const int status = tcp_client_->connect(
        host_.c_str(), tcp_port, [weak, tcp_port](int connect_status) {
          if (auto self = weak.lock()) {
            self->on_tcp_connected(connect_status, tcp_port);
          }
        });
    if (status != 0) {
      std::fprintf(stderr, "direct proxy: TCP connect failed to start (%d)\n",
                   status);
      tcp_client_.reset();
      tcp_state_.reset();
      fallback_from_tcp();
    }
  }

  void on_tcp_connected(int status, int tcp_port) {
    if (closed_) return;
    if (status != 0 || !tcp_client_) {
      std::fprintf(stderr,
                   "direct proxy: TCP connect failed (%d); trying next\n",
                   status);
      tcp_client_.reset();
      tcp_state_.reset();
      fallback_from_tcp();
      return;
    }
    std::shared_ptr<tcp_link_state> state = tcp_state_;
    auto link = std::make_shared<stream_transport>(
        [state](const uint8_t* data, std::size_t len) {
          if (state->closed || state->client == nullptr) return false;
          if (state->queued + len > kMaxTcpBacklogBytes) {
            tcp_link_fail(state, "TCP send backlog exceeded");
            return false;
          }
          state->queue.emplace_back(reinterpret_cast<const char*>(data), len);
          state->queued += len;
          tcp_link_pump(state);
          return true;
        },
        [state](int, const std::string&) {
          if (state->closed) return;
          state->closed = true;
          state->queue.clear();
          state->queued = 0;
          uvcpp::uvcpp_tcp_client* active = state->client;
          state->client = nullptr;
          if (active != nullptr) active->close();
        },
        "tcp", host_, static_cast<unsigned int>(tcp_port));
    state->link = link;

    uvcpp::uvcpp_tcp_client* client = tcp_client_.get();
    client->set_on_close([state] {
      state->closed = true;
      state->client = nullptr;
      state->queue.clear();
      state->queued = 0;
      if (state->link) state->link->peer_closed("peer closed");
    });
    client->read_start_events(
        [state](uvcpp::uvcpp_tcp_client&, const uvcpp::net_read_result& r) {
          if (r.event != uvcpp::net_read_event::DATA) return;
          if (!state->closed && state->link) {
            state->link->feed(reinterpret_cast<const uint8_t*>(r.data), r.size);
          }
        });

    attach_transport(link);
    flush_pending();
  }

  void attach_transport(std::shared_ptr<session_transport> transport) {
    transport_ = std::move(transport);
    std::weak_ptr<direct_proxy> weak = weak_from_this();
    transport_->set_handlers(
        [weak](const std::string& text) {
          if (auto self = weak.lock()) {
            self->flush_pending();
            if (text.find("hello_required") != std::string::npos &&
                !self->hello_resent_ && !self->hello_text_.empty()) {
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
            self->deliver_binary(data, len);
          }
        },
        [weak](int code, const std::string& reason) {
          if (auto self = weak.lock()) {
            self->closed_ = true;
            self->stop_worker();
            if (self->browser_ != nullptr) {
              self->browser_->close(static_cast<uvcpp::ws_close_code>(code),
                                    reason);
            }
            self->browser_ = nullptr;
          }
        });
  }

#if MOLINK_HAVE_H2_WS
  /// HTTP/2 WebSocket (RFC 8441) on the host's web port. h2c prior knowledge
  /// is used because the direct web port is plain; the extended CONNECT is
  /// opened once the server's SETTINGS arrive.
  bool start_h2_upstream() {
    if (closed_ || transport_ || loop_ == nullptr || !http2_available_) {
      return false;
    }
    h2_client_ = std::make_unique<uvcpp::uvcpp_tcp_client>(loop_);
    h2_deadline_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::weak_ptr<direct_proxy> weak = weak_from_this();
    const int status = h2_client_->connect(
        host_.c_str(), port_, [weak](int connect_status) {
          if (auto self = weak.lock()) self->on_h2_connected(connect_status);
        });
    if (status != 0) {
      std::fprintf(stderr,
                   "direct proxy: HTTP/2 connect failed to start (%d)\n",
                   status);
      stop_h2();
      return false;
    }
    return true;
  }

  void on_h2_connected(int status) {
    if (closed_ || transport_) {
      stop_h2();
      return;
    }
    if (status != 0 || !h2_client_) {
      std::fprintf(stderr,
                   "direct proxy: HTTP/2 TCP connect failed (%d); using WS\n",
                   status);
      stop_h2();
      start_ws_upstream();
      return;
    }
    h2_conn_ = std::make_unique<uvcpp::uvcpp_h2_connection>(
        h2_client_.get(), /*server_side=*/false);
    uvcpp::uvcpp_h2_session::callbacks h2_callbacks;
    uvcpp::uvcpp_h2_connection::callbacks connection_callbacks;
    std::weak_ptr<direct_proxy> weak = weak_from_this();
    connection_callbacks.on_disconnect = [weak](uvcpp::uvcpp_h2_connection&) {
      if (auto self = weak.lock()) self->on_h2_disconnected();
    };
    if (h2_conn_->start(h2_callbacks, connection_callbacks) != 0) {
      std::fprintf(stderr, "direct proxy: HTTP/2 session start failed\n");
      stop_h2();
      start_ws_upstream();
      return;
    }
    h2_timer_ = std::make_unique<uvcpp::uvcpp_timer>(loop_);
    if (h2_timer_->init() != 0) {
      std::fprintf(stderr, "direct proxy: HTTP/2 timer init failed\n");
      stop_h2();
      start_ws_upstream();
      return;
    }
    h2_timer_->start(
        [weak](uvcpp::uvcpp_timer*) {
          if (auto self = weak.lock()) self->try_h2_connect();
        },
        20, 20);
  }

  void try_h2_connect() {
    if (closed_ || transport_ || !h2_conn_) {
      stop_h2();
      return;
    }
    if (std::chrono::steady_clock::now() > h2_deadline_) {
      std::fprintf(stderr,
                   "direct proxy: HTTP/2 handshake timed out; using WS\n");
      stop_h2();
      start_ws_upstream();
      return;
    }
    const std::string authority = host_ + ":" + std::to_string(port_);
    auto link = std::make_shared<h2_ws_transport>(
        host_, static_cast<unsigned int>(port_));
    std::weak_ptr<h2_ws_transport> weak_link = link;
    uvcpp::uvcpp_h2_ws_connection::callbacks callbacks;
    callbacks.on_text = [weak_link](const std::string& text) {
      if (auto transport = weak_link.lock()) transport->deliver_text(text);
    };
    callbacks.on_binary = [weak_link](const uint8_t* data, std::size_t len) {
      if (auto transport = weak_link.lock()) {
        transport->deliver_binary(data, len);
      }
    };
    callbacks.on_close = [weak_link](uvcpp::ws_close_code code,
                                     const std::string& reason) {
      if (auto transport = weak_link.lock()) {
        transport->link_closed(static_cast<int>(code), reason);
      }
    };
    callbacks.on_error = [weak_link](int code, const std::string& reason) {
      if (auto transport = weak_link.lock()) {
        transport->link_closed(code, reason);
      }
    };
    auto ws = uvcpp::uvcpp_h2_ws_connection::connect(
        *h2_conn_, "/ws/session", authority, callbacks);
    if (ws == nullptr) return;  // SETTINGS not received yet; retry.
    if (h2_timer_) {
      h2_timer_->stop();
      h2_timer_.reset();
    }
    h2_ws_ = std::move(ws);
    h2_link_ = link;
    link->attach(h2_ws_.get());
    attach_transport(link);
    flush_pending();
  }

  void on_h2_disconnected() {
    if (closed_) return;
    if (!h2_client_ && !h2_conn_) return;
    if (h2_link_ && transport_ == h2_link_) {
      h2_link_->link_closed(transport_close::going_away,
                            "HTTP/2 connection closed");
      return;
    }
    if (!transport_) {
      stop_h2();
      start_ws_upstream();
    }
  }

  void stop_h2() {
    if (h2_timer_) {
      h2_timer_->stop();
      h2_timer_.reset();
    }
    h2_ws_.reset();
    h2_conn_.reset();
    std::unique_ptr<uvcpp::uvcpp_tcp_client> client = std::move(h2_client_);
    h2_client_.reset();
    if (client) client->close();
    h2_link_.reset();
  }
#endif

  void start_ws_upstream() {
    if (closed_ || transport_) return;
    auto relay = std::make_shared<relay_transport>(
        "ws://" + format_url_host(host_) + ":" + std::to_string(port_) +
            "/ws/session",
        std::string(), post_, /*relay_protocol=*/false);
    std::weak_ptr<direct_proxy> weak = weak_from_this();
    relay->set_ready_handler([weak] {
      if (auto self = weak.lock()) self->flush_pending();
    });
    attach_transport(relay);
    relay->start();
  }

#if UVCPP_QUIC_ENABLE
  void start_quic_upstream(int quic_port) {
    if (closed_ || transport_ || loop_ == nullptr) {
      start_ws_upstream();
      return;
    }
    // The discovery answer arrived over plain HTTP, so there is no
    // authenticated material to pin yet (the welcome's TLS fingerprint comes
    // only after a session exists). This leg is therefore encrypted but not
    // peer-verified, exactly like the plain `ws://` fallback it replaces; a
    // later device-ID integration can pin through the signaling exchange.
    quic_tls_ = std::make_shared<uvcpp::uvcpp_ssl_context>(
        uvcpp::tls_mode::CLIENT);
    if (!pinned_ca_.empty() && quic_tls_->load_ca_file(pinned_ca_)) {
      quic_tls_->set_verify_mode(uvcpp::tls_verify_mode::PEER);
    } else {
      // Older hosts do not publish a certificate; keep the encrypted but
      // unverified behaviour rather than refusing the session.
      quic_tls_->set_verify_mode(uvcpp::tls_verify_mode::NONE);
    }
    quic_tls_->set_alpn_protos({"molink"});
    quic_client_ = std::make_shared<uvcpp::uvcpp_quic_client>(loop_);
    quic_client_->set_ssl_context(quic_tls_.get());
    quic_client_->set_alpn_protos({"molink"});
    std::weak_ptr<direct_proxy> weak = weak_from_this();
    const int status = quic_client_->connect(
        host_.c_str(), quic_port,
        [weak](int connect_status) {
          if (auto self = weak.lock()) self->on_quic_connected(connect_status);
        });
    if (status != 0) {
      std::fprintf(stderr, "direct proxy: QUIC connect failed to start (%d)\n",
                   status);
      fallback_from_quic();
    }
  }

  void on_quic_connected(int status) {
    if (closed_) return;
    uvcpp::uvcpp_quic_connection* connection =
        quic_client_ ? quic_client_->connection() : nullptr;
    if (status != 0 || connection == nullptr) {
      std::fprintf(stderr,
                   "direct proxy: QUIC handshake failed (%d); TCP next\n",
                   status);
      fallback_from_quic();
      return;
    }
    const int64_t stream_id = connection->open_stream(true);
    if (stream_id < 0) {
      std::fprintf(stderr, "direct proxy: QUIC open_stream failed (%lld)\n",
                   static_cast<long long>(stream_id));
      connection->close();
      fallback_from_quic();
      return;
    }

    std::weak_ptr<direct_proxy> weak = weak_from_this();
    uvcpp::uvcpp_quic_connection::callbacks callbacks;
    callbacks.on_read = [weak, stream_id](
                            uvcpp::uvcpp_quic_connection&,
                            int64_t read_stream,
                            const uvcpp::net_read_result& result) {
      if (read_stream != stream_id) return;
      auto self = weak.lock();
      if (!self || !self->quic_link_) return;
      if (result.event == uvcpp::net_read_event::DATA) {
        self->quic_link_->feed(
            reinterpret_cast<const uint8_t*>(result.data), result.size);
      } else if (result.event == uvcpp::net_read_event::PEER_CLOSED) {
        self->quic_link_->peer_closed("host closed the QUIC stream");
      } else {
        self->quic_link_->peer_closed("QUIC stream read error");
      }
    };
    callbacks.on_close = [weak](uvcpp::uvcpp_quic_connection&, int code) {
      auto self = weak.lock();
      if (!self || !self->quic_link_) return;
      self->quic_link_->peer_closed("QUIC connection closed (" +
                                    std::to_string(code) + ")");
    };
    connection->set_callbacks(callbacks);

    auto link = std::make_shared<stream_transport>(
        [connection, stream_id](const uint8_t* data, std::size_t len) {
          return connection->write_stream(
                     stream_id, reinterpret_cast<const char*>(data), len,
                     false) == 0;
        },
        [connection](int, const std::string&) { connection->close(); });
    quic_link_ = link;
    attach_transport(link);
    flush_pending();
  }
#endif

  void on_browser_text(const std::string& text) {
    if (closed_) return;
    std::string outgoing = text;
    if (text.find("\"hello\"") != std::string::npos) {
      outgoing = inject_av1(text);
      hello_text_ = outgoing;
    }
    if (!transport_ || !transport_->is_open()) {
      pending_text_.push_back(outgoing);
      return;
    }
    transport_->send_text(outgoing);
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
    stop_worker();
    pending_text_.clear();
    pending_binary_.clear();
    if (transport_) transport_->close(transport_close::normal, "browser closed");
    transport_.reset();
#if UVCPP_QUIC_ENABLE
    // Also stop a QUIC handshake that has not delivered a transport yet.
    if (quic_client_ && quic_client_->connection() != nullptr) {
      quic_client_->connection()->close();
    }
#endif
#if MOLINK_HAVE_H2_WS
    stop_h2();
#endif
    if (tcp_client_) tcp_client_->close();
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

  std::string inject_av1(const std::string& text) {
    if (!av1_decode_available() || !post_) return text;
    try {
      nlohmann::json hello = nlohmann::json::parse(text);
      if (!hello.is_object()) return text;
      hello["av1"] = true;
      hello["av1_stream"] = true;
      return hello.dump();
    } catch (...) {
      return text;
    }
  }

  // ---- AV1 transcoding (same contract as the device-ID proxy) ----------

  static bool packet_is_av1(const std::vector<uint8_t>& bytes) {
    return bytes.size() >=
               protocol::kPacketHeaderSize + protocol::kFrameHeaderSize &&
           std::memcmp(bytes.data(), protocol::kMagic,
                       sizeof(protocol::kMagic)) == 0 &&
           bytes[5] == static_cast<uint8_t>(protocol::packet_type::frame) &&
           (bytes[protocol::kPacketHeaderSize + 6] ==
                static_cast<uint8_t>(protocol::codec::av1) ||
            bytes[protocol::kPacketHeaderSize + 6] ==
                static_cast<uint8_t>(protocol::codec::av1_stream));
  }

  void start_worker() {
    if (worker_started_ || !post_ || !av1_decode_available()) return;
    worker_started_ = true;
    worker_ = std::thread([this] { worker_loop(); });
  }

  void stop_worker() {
    if (!worker_started_) return;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      stopping_ = true;
    }
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    worker_started_ = false;
    std::lock_guard<std::mutex> lock(queue_mutex_);
    binary_queue_.clear();
  }

  void deliver_binary(const uint8_t* data, std::size_t len) {
    if (closed_ || browser_ == nullptr || data == nullptr) return;
    // Every packet here crossed the network to the host, whether or not the
    // page will see its bytes: an AV1 patch is transcoded, so the page's own
    // byte counter would show the decoded (several times larger) size.
    wire_window_.fetch_add(len);
    if (!worker_started_) {
      browser_->send_binary(reinterpret_cast<const char*>(data), len);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      while (binary_queue_.size() >= 8) binary_queue_.pop_front();
      binary_queue_.emplace_back(data, data + len);
    }
    queue_cv_.notify_one();
  }

  void worker_loop() {
    for (;;) {
      std::vector<uint8_t> packet;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] {
          return stopping_ || !binary_queue_.empty();
        });
        if (stopping_ && binary_queue_.empty()) return;
        packet = std::move(binary_queue_.front());
        binary_queue_.pop_front();
      }
      const bool av1 = packet_is_av1(packet);
      // A patch follows a tile frame (tile-refresh flag); an AV1-only picture
      // does not. The page uses this to label the stream as AV1 or AV1+tiles.
      const bool full_frame =
          av1 && (packet[6] & protocol::kPacketFlagTileRefresh) == 0;
      const std::size_t wire = packet.size();
      std::vector<uint8_t> out = transcode(std::move(packet));
      if (out.empty()) continue;
      const std::size_t decoded = out.size();
      bool announce = false;
      std::size_t wire_rate = 0;
      if (av1) {
        const auto now = std::chrono::steady_clock::now();
        if (last_stat_.time_since_epoch().count() == 0 ||
            now - last_stat_ >= std::chrono::seconds(1)) {
          const double seconds =
              window_start_.time_since_epoch().count() == 0
                  ? 0.0
                  : std::chrono::duration<double>(now - window_start_).count();
          const std::size_t bytes = wire_window_.exchange(0);
          if (seconds > 0.2) {
            wire_rate = static_cast<std::size_t>(
                static_cast<double>(bytes) / seconds);
          }
          window_start_ = now;
          last_stat_ = now;
          announce = true;
        }
      }
      std::weak_ptr<direct_proxy> weak = weak_from_this();
      std::vector<uint8_t> bytes = std::move(out);
      post_([weak, bytes = std::move(bytes), announce, wire, decoded,
             full_frame, wire_rate] {
        if (auto self = weak.lock()) {
          if (announce)
            self->send_codec_stat(wire, decoded, full_frame, wire_rate);
          self->send_to_browser(bytes.data(), bytes.size());
        }
      });
    }
  }

  void send_to_browser(const uint8_t* data, std::size_t len) {
    if (closed_ || browser_ == nullptr) return;
    browser_->send_binary(reinterpret_cast<const char*>(data), len);
  }

  void send_codec_stat(std::size_t wire, std::size_t decoded,
                       bool full_frame, std::size_t wire_rate) {
    if (closed_ || browser_ == nullptr) return;
    const std::string text = "{\"type\":\"codec\",\"av1\":true,\"wire\":" +
                             std::to_string(wire) + ",\"decoded\":" +
                             std::to_string(decoded) +
                             (full_frame ? ",\"full\":true" : "") +
                             ",\"wire_rate\":" + std::to_string(wire_rate) +
                             "}";
    browser_->send_text(text.data(), text.size());
  }

  std::vector<uint8_t> transcode(std::vector<uint8_t> bytes) {
    protocol::packet packet;
    std::string error;
    if (!protocol::decode_packet(bytes.data(), bytes.size(), packet, error)) {
      return bytes;
    }
    if (packet.type != static_cast<uint8_t>(protocol::packet_type::frame) ||
        packet.payload.size() < protocol::kFrameHeaderSize) {
      return bytes;
    }
    const uint8_t codec = packet.payload[6];
    if (codec == static_cast<uint8_t>(protocol::codec::av1_stream)) {
      // One full-screen picture of a low-delay stream: decode it with the
      // session's decoder, then hand the page a full-screen tile refresh, so
      // it sees no difference between a stream frame and a tile repair.
      protocol::parsed_frame frame;
      if (!protocol::parse_frame_payload(packet.payload.data(),
                                         packet.payload.size(), frame, error)) {
        return {};
      }
      if (frame.tiles.empty()) return {};
      const protocol::tile_view& view = frame.tiles[0];
      const bool keyframe =
          (packet.payload[7] & protocol::kFrameFlagKeyframe) != 0;
      std::vector<uint8_t> bgra;
      int width = 0;
      int height = 0;
      const auto decode_start = std::chrono::steady_clock::now();
      if (!stream_decoder_.decode(view.data, view.size, keyframe, bgra, width,
                                  height, error)) {
        std::fprintf(stderr, "direct proxy: av1 stream decode failed: %s\n",
                     error.c_str());
        // The reference chain is broken; ask the host for a keyframe. The
        // transport owns QUIC state and is only safe on the loop thread,
        // so the request is posted there: writing it from this decoder
        // thread crashed ngtcp2. One a second is enough; a burst of failed
        // frames must not restart the stream in a loop.
        const auto request_now = std::chrono::steady_clock::now();
        if (last_stream_keyframe_request_.time_since_epoch().count() == 0 ||
            request_now - last_stream_keyframe_request_ >=
                std::chrono::seconds(1)) {
          last_stream_keyframe_request_ = request_now;
          std::weak_ptr<direct_proxy> weak_self = weak_from_this();
          const auto post = post_;
          if (post) {
            post([weak_self] {
              auto self = weak_self.lock();
              if (!self || !self->transport_) return;
              static const char kRequest[] =
                  "{\"type\":\"request-keyframe\"}";
              self->transport_->send_text(kRequest);
            });
          }
        }
        return {};
      }
      // The page paints full-screen frames; forwarding more than ten a
      // second only adds latency and inflate work. Every frame is still
      // decoded so the reference chain stays valid.
      const auto now = std::chrono::steady_clock::now();
      if (last_stream_forward_.time_since_epoch().count() != 0 &&
          now - last_stream_forward_ < std::chrono::milliseconds(100)) {
        return {};
      }
      last_stream_forward_ = now;
      protocol::frame_header header;
      header.frame_seq = frame.header.frame_seq;
      header.codec = static_cast<uint8_t>(protocol::codec::rgb565);
      header.flags = static_cast<uint8_t>(
          keyframe ? (protocol::kFrameFlagKeyframe | protocol::kFrameFlagPartial)
                   : protocol::kFrameFlagPartial);
      header.screen_w = frame.header.screen_w;
      header.screen_h = frame.header.screen_h;
      header.tile_w = frame.header.tile_w;
      header.tile_h = frame.header.tile_h;
      protocol::tile_blob tile;
      tile.x = 0;
      tile.y = 0;
      tile.w = static_cast<uint16_t>(
          std::min<int>(width, header.screen_w > 0 ? header.screen_w : width));
      tile.h = static_cast<uint16_t>(
          std::min<int>(height, header.screen_h > 0 ? header.screen_h : height));
      tile.flags = protocol::kTileFlagChanged;
      // A stream frame is a whole screen: pack it as RGB565, which halves
      // everything the core deflates and the page inflates on every frame.
      tile.data.resize(static_cast<std::size_t>(tile.w) * tile.h * 2);
      for (int row = 0; row < tile.h; ++row) {
        const uint8_t* src =
            bgra.data() + static_cast<std::size_t>(row) * width * 4;
        uint8_t* dst =
            tile.data.data() + static_cast<std::size_t>(row) * tile.w * 2;
        for (int x = 0; x < tile.w; ++x) {
          const uint8_t* px = src + static_cast<std::size_t>(x) * 4;
          const uint16_t value = static_cast<uint16_t>(
              (static_cast<uint16_t>(px[2] >> 3) << 11) |
              (static_cast<uint16_t>(px[1] >> 2) << 5) |
              static_cast<uint16_t>(px[0] >> 3));
          dst[x * 2] = static_cast<uint8_t>(value & 0xff);
          dst[x * 2 + 1] = static_cast<uint8_t>(value >> 8);
        }
      }
      std::vector<protocol::tile_blob> tiles;
      tiles.push_back(std::move(tile));
      const auto deflate_start = std::chrono::steady_clock::now();
      std::vector<std::vector<uint8_t>> packed(tiles.size());
      if (compression_available()) {
        std::string deflate_error;
        for (std::size_t i = 0; i < tiles.size(); ++i) {
          if (!deflate_buffer(tiles[i].data.data(), tiles[i].data.size(), 1,
                              packed[i], deflate_error)) {
            return {};
          }
          tiles[i].data = std::move(packed[i]);
        }
        header.codec = static_cast<uint8_t>(protocol::codec::deflate_rgb565);
      }
      std::vector<uint8_t> payload;
      if (!protocol::build_frame_payload(header, tiles, payload, error)) {
        return {};
      }
      if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
        const double decode_ms = std::chrono::duration<double, std::milli>(
                                     deflate_start - decode_start)
                                     .count();
        const double deflate_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() -
                                      deflate_start)
                                      .count();
        std::fprintf(stderr,
                     "stream frame: decode %.1f ms deflate %.1f ms wire %zu B\n",
                     decode_ms, deflate_ms, payload.size());
      }
      std::vector<uint8_t> out;
      if (!protocol::encode_packet_into(
              out, packet.type,
              static_cast<uint16_t>(protocol::kPacketFlagTileRefresh),
              packet.session, packet.seq, packet.ts_us, payload.data(),
              payload.size(), error)) {
        return {};
      }
      return out;
    }
    if (codec != static_cast<uint8_t>(protocol::codec::av1)) {
      return bytes;
    }
    std::vector<protocol::tile_blob> tiles;
    if (!decode_av1_frame_payload(packet.payload.data(), packet.payload.size(),
                                  tiles, error)) {
      std::fprintf(stderr, "direct proxy: av1 decode failed: %s\n",
                   error.c_str());
      return {};
    }
    protocol::frame_header header;
    header.frame_seq = static_cast<uint32_t>(packet.payload[0]) |
                       (static_cast<uint32_t>(packet.payload[1]) << 8) |
                       (static_cast<uint32_t>(packet.payload[2]) << 16) |
                       (static_cast<uint32_t>(packet.payload[3]) << 24);
    header.codec = static_cast<uint8_t>(protocol::codec::bgra);
    // A decoded AV1 keyframe is full-screen BGRA (tens of megabytes on a 4K
    // desktop). Sending it raw over the local WebSocket blocks the app loop
    // and forces the page to handle a huge message; deflate it on the worker
    // instead. The page already inflates deflate_bgra asynchronously.
    // Tiny patches are cheaper to send raw: deflating a few kilobytes costs
    // more than it saves, and the page paints either form the same way.
    std::size_t raw_total = 0;
    for (const auto& tile : tiles) raw_total += tile.data.size();
    constexpr std::size_t kMinDeflateBytes = 96 * 1024;
    if (compression_available() && !tiles.empty() &&
        raw_total >= kMinDeflateBytes) {
      std::vector<std::vector<uint8_t>> packed(tiles.size());
      bool compressed = true;
      std::string deflate_error;
      for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (tiles[i].data.empty() ||
            !deflate_buffer(tiles[i].data.data(), tiles[i].data.size(), 1,
                            packed[i], deflate_error) ||
            packed[i].size() >= tiles[i].data.size()) {
          compressed = false;
          break;
        }
      }
      if (compressed) {
        for (std::size_t i = 0; i < tiles.size(); ++i) {
          tiles[i].data = std::move(packed[i]);
        }
        header.codec = static_cast<uint8_t>(protocol::codec::deflate_bgra);
      }
    }
    header.flags = packet.payload[7];
    header.screen_w = static_cast<uint16_t>(
        packet.payload[8] | (packet.payload[9] << 8));
    header.screen_h = static_cast<uint16_t>(
        packet.payload[10] | (packet.payload[11] << 8));
    header.tile_w = static_cast<uint16_t>(
        packet.payload[12] | (packet.payload[13] << 8));
    header.tile_h = static_cast<uint16_t>(
        packet.payload[14] | (packet.payload[15] << 8));
    std::vector<uint8_t> payload;
    if (!protocol::build_frame_payload(header, tiles, payload, error)) {
      return {};
    }
    std::vector<uint8_t> out;
    if (!protocol::encode_packet_into(out, packet.type, packet.flags,
                                      packet.session, packet.seq,
                                      packet.ts_us, payload.data(),
                                      payload.size(), error)) {
      return {};
    }
    return out;
  }

  uvcpp::uvcpp_ws_connection* browser_ = nullptr;
  std::string host_;
  int port_ = 0;
  uvcpp::uvcpp_loop* loop_ = nullptr;
  std::function<void(std::function<void()>)> post_;
  std::shared_ptr<session_transport> transport_;
  int tcp_port_ = 0;
  bool http2_available_ = false;
  std::string pinned_ca_;
  std::shared_ptr<uvcpp::uvcpp_ssl_context> tcp_tls_;
  std::unique_ptr<uvcpp::uvcpp_tcp_client> tcp_client_;
  std::shared_ptr<tcp_link_state> tcp_state_;
#if MOLINK_HAVE_H2_WS
  // Destruction order: the WS borrows the h2 connection, which borrows the
  // TCP client, so declare them in that order (reverse destruction).
  std::unique_ptr<uvcpp::uvcpp_tcp_client> h2_client_;
  std::unique_ptr<uvcpp::uvcpp_h2_connection> h2_conn_;
  std::unique_ptr<uvcpp::uvcpp_h2_ws_connection> h2_ws_;
  std::shared_ptr<h2_ws_transport> h2_link_;
  std::unique_ptr<uvcpp::uvcpp_timer> h2_timer_;
  std::chrono::steady_clock::time_point h2_deadline_{};
#endif
#if UVCPP_QUIC_ENABLE
  std::shared_ptr<uvcpp::uvcpp_ssl_context> quic_tls_;
  std::shared_ptr<uvcpp::uvcpp_quic_client> quic_client_;
  std::shared_ptr<stream_transport> quic_link_;
#endif
  std::deque<std::string> pending_text_;
  std::deque<std::vector<uint8_t>> pending_binary_;
  std::string hello_text_;
  bool hello_resent_ = false;
  bool flushed_ = false;
  bool closed_ = false;

  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<std::vector<uint8_t>> binary_queue_;
  bool worker_started_ = false;
  bool stopping_ = false;
  std::chrono::steady_clock::time_point last_stat_{};
  /// The low-delay stream keeps reference frames, so one decoder lives for the
  /// whole session.
  av1_stream_decoder stream_decoder_;
  /// Last keyframe request for a broken stream (one a second).
  std::chrono::steady_clock::time_point last_stream_keyframe_request_{};
  /// Last time a stream frame was forwarded to the page (rate limit).
  std::chrono::steady_clock::time_point last_stream_forward_{};
  /// Host bytes received since the last codec stat (the true wire rate) and
  /// the start of that window. The window start is only touched by the worker.
  std::atomic<std::size_t> wire_window_{0};
  std::chrono::steady_clock::time_point window_start_{};
};

}  // namespace

uvcpp::uvcpp_web_ws_handler make_direct_handler(
    std::function<void(std::function<void()>)> post, uvcpp::uvcpp_loop* loop) {
  return [post = std::move(post), loop](uvcpp::uvcpp_web_ws_request& request) {
    uvcpp::uvcpp_ws_connection* conn = request.connection();
    if (conn == nullptr) return;
    if (!is_loopback_peer(request.peer_ip())) {
      conn->close(uvcpp::ws_close_code::POLICY_VIOLATION,
                  "the direct proxy is local-only");
      return;
    }
    const std::string* host = request.query("host");
    const std::string* port = request.query("port");
    if (host == nullptr || host->empty() || port == nullptr) {
      conn->close(uvcpp::ws_close_code::POLICY_VIOLATION,
                  "host and port are required");
      return;
    }
    const int port_number = std::atoi(port->c_str());
    if (port_number <= 0 || port_number > 65535) {
      conn->close(uvcpp::ws_close_code::POLICY_VIOLATION,
                  "port must be 1..65535");
      return;
    }
    auto proxy =
        std::make_shared<direct_proxy>(conn, *host, port_number, loop, post);
    proxy->start();
  };
}

}  // namespace molink
