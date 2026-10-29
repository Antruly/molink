#include "transport/connect_proxy.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/av1_frame.h"
#include "capture/jpeg_encode.h"
#include "surface/frame_surface.h"
#include "compression.h"
#include "protocol/frame_packet.h"
#include "transport/switchable_transport.h"

namespace molink {
namespace {

class connect_proxy final
    : public std::enable_shared_from_this<connect_proxy> {
 public:
  connect_proxy(uvcpp::uvcpp_ws_connection* browser,
                std::shared_ptr<p2p_manager> manager, std::string target,
                bool relay_only,
                std::function<void(std::function<void()>)> post)
      : browser_(browser), manager_(std::move(manager)),
        target_(std::move(target)), relay_only_(relay_only),
        post_(std::move(post)) {}

  ~connect_proxy() { stop_worker(); }

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
    start_worker();
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
    std::string outgoing = text;
    if (text.find("\"hello\"") != std::string::npos) {
      outgoing = inject_av1(text);
      // Keep the hello so it can be replayed if the first datagram raced the
      // target's session controller and was dropped.
      hello_text_ = outgoing;
    } else if (text.find("\"quality\"") != std::string::npos) {
      note_quality(text);
    }
    if (!transport_ || !transport_->is_open()) {
      pending_text_.push_back(outgoing);
      return;
    }
    transport_->send_text(outgoing);
  }

  /// The page cannot decode AV1 on every platform (WebKitGTK's WebCodecs
  /// rejects it), so the core owns that capability: advertise it in the hello
  /// the target sees, and only then does the host produce AV1 patches.
  std::string inject_av1(const std::string& text) {
    // AV1 is decoded by the page (WebCodecs), so the host may only stream AV1
    // when the page says it can. The proxy no longer decodes AV1 itself and
    // has no fallback for a page without the codec: such a viewer simply is
    // not offered AV1 and the host picks H.264/VP9/tiles instead. Single-frame
    // AV1 (patches) is retired on the host, so `av1` is cleared either way.
    try {
      nlohmann::json hello = nlohmann::json::parse(text);
      if (!hello.is_object()) return text;
      bool native = false;
      const auto it = hello.find("av1_native");
      if (it != hello.end() && it->is_boolean()) native = it->get<bool>();
      hello.erase("av1");
      if (native) {
        hello["av1_stream"] = true;
        if (std::getenv("MOLINK_AV1_LOG") != nullptr) {
          std::fprintf(stderr, "proxy: injected av1_stream into hello\n");
        }
      } else {
        hello.erase("av1_stream");
      }
      return hello.dump();
    } catch (...) {
      return text;
    }
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
  }

  /// The page pushes the quality preset through this proxy; stream frames
  /// follow it (a sharp preset gets a sharper JPEG).
  void note_quality(const std::string& text) {
    try {
      const auto document = nlohmann::json::parse(text);
      if (!document.is_object()) return;
      if (document.value("type", std::string()) != "quality") return;
      const std::string mode = document.value("mode", std::string());
      if (mode == "smooth" || mode == "game") {
        stream_quality_ = 72;
      } else if (mode == "turbo") {
        stream_quality_ = 70;
      } else if (mode == "balanced") {
        stream_quality_ = 80;
      } else if (mode == "quality" || mode == "ultra" || mode == "lossless") {
        stream_quality_ = 88;
      }
    } catch (...) {
      // A malformed control message is not this proxy's business.
    }
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

  /// `mode` is the legacy relay/p2p flag; `path` and `protocol` are the two
  /// axes the UI labels separately (a relayed QUIC session is path=relay,
  /// protocol=quic).
  void send_transport(const char* mode) {
    if (browser_ == nullptr) return;
    const char* path = transport_ ? transport_->path() : mode;
    const char* protocol = transport_ ? transport_->name() : "websocket";
    const std::string message =
        std::string("{\"type\":\"transport\",\"mode\":\"") + mode +
        "\",\"path\":\"" + path + "\",\"protocol\":\"" + protocol + "\"}";
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
    // The target controller is created as soon as its ICE pair completes and
    // is already waiting for hello, so send the buffered hello now.
    flush_pending();
  }

  void on_error(const std::string& error) {
    if (closed_ || browser_ == nullptr) return;
    closed_ = true;
    stop_worker();
    browser_->close(uvcpp::ws_close_code::POLICY_VIOLATION,
                    error.empty() ? "p2p unavailable" : error);
    browser_ = nullptr;
  }

  // ---- AV1 transcoding -------------------------------------------------
  //
  // Only frames whose header codec is av1 need work; everything else is
  // forwarded untouched. All binary packets go through the single worker so
  // frames stay in order; text (pings, welcome) bypasses it.

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
      // A decoder slower than the link must not grow memory without bound;
      // drop the oldest packet, the next frame carries newer pixels.
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
      std::weak_ptr<connect_proxy> weak = weak_from_this();
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

  /// Tells the page that the client core is decoding AV1 patches, so the UI
  /// can show an indicator; the page itself never sees the AV1 bytes.
  void send_codec_stat(std::size_t wire, std::size_t decoded, bool full_frame,
                       std::size_t wire_rate) {
    if (closed_ || browser_ == nullptr) return;
    const std::string text = "{\"type\":\"codec\",\"av1\":true,\"wire\":" +
                             std::to_string(wire) + ",\"decoded\":" +
                             std::to_string(decoded) +
                             (full_frame ? ",\"full\":true" : "") +
                             ",\"wire_rate\":" + std::to_string(wire_rate) +
                             "}";
    browser_->send_text(text.data(), text.size());
  }

  void send_to_browser(const uint8_t* data, std::size_t len) {
    if (closed_ || browser_ == nullptr) return;
    browser_->send_binary(reinterpret_cast<const char*>(data), len);
  }

  /// Rewrites an AV1 patch into the BGRA tiles the page already knows how to
  /// paint. Non-AV1 packets are returned unchanged; a patch that fails to
  /// decode is dropped (the next frame carries newer pixels).
  std::vector<uint8_t> transcode(std::vector<uint8_t> bytes) {
    // AV1 is decoded by the page itself through WebCodecs; the proxy forwards
    // every packet unchanged. The old path decoded AV1 in the core and
    // re-encoded each picture as JPEG for the page, which capped the stream at
    // 20 fps and made AV1 feel slower than H.264. No decode, no JPEG, no
    // repaint throttle.
    return bytes;
  }

  uvcpp::uvcpp_ws_connection* browser_ = nullptr;
  std::shared_ptr<p2p_manager> manager_;
  std::string target_;
  bool relay_only_ = false;
  std::function<void(std::function<void()>)> post_;
  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<std::vector<uint8_t>> binary_queue_;
  bool worker_started_ = false;
  bool stopping_ = false;
  std::chrono::steady_clock::time_point last_stat_{};
  /// The low-delay stream keeps reference frames per session.
  av1_stream_decoder stream_decoder_;
  /// JPEG quality for stream frames, following the viewer's preset.
  int stream_quality_ = 80;
  /// Last keyframe request for a broken stream (one a second).
  std::chrono::steady_clock::time_point last_stream_keyframe_request_{};
  /// Last time a stream frame was forwarded to the page (rate limit).
  std::chrono::steady_clock::time_point last_stream_forward_{};
  /// Host bytes received since the last codec stat (the true wire rate) and
  /// the start of that window. The window start is only touched by the worker.
  std::atomic<std::size_t> wire_window_{0};
  std::chrono::steady_clock::time_point window_start_{};
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
    std::shared_ptr<p2p_manager> manager,
    std::function<void(std::function<void()>)> post) {
  return [manager, post = std::move(post)](uvcpp::uvcpp_web_ws_request& request) {
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
    auto proxy = std::make_shared<connect_proxy>(conn, manager, *target,
                                                 relay_only, post);
    proxy->start();
  };
}

}  // namespace molink
