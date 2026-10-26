#include "transport/direct_proxy.h"

#include <nlohmann/json.hpp>

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
#include "protocol/frame_packet.h"
#include "transport/relay_transport.h"
#include "util.h"

namespace molink {
namespace {

class direct_proxy final : public std::enable_shared_from_this<direct_proxy> {
 public:
  direct_proxy(uvcpp::uvcpp_ws_connection* browser, std::string host, int port,
               std::function<void(std::function<void()>)> post)
      : browser_(browser),
        host_(std::move(host)),
        port_(port),
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
    transport_ = std::make_shared<relay_transport>(
        "ws://" + format_url_host(host_) + ":" + std::to_string(port_) +
            "/ws/session",
        std::string(), post_, /*relay_protocol=*/false);
    std::weak_ptr<direct_proxy> weak = weak_from_this();
    transport_->set_ready_handler([weak] {
      if (auto self = weak.lock()) self->flush_pending();
    });
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
    transport_->start();
  }

 private:
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
           bytes[protocol::kPacketHeaderSize + 6] ==
               static_cast<uint8_t>(protocol::codec::av1);
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
      const std::size_t wire = packet.size();
      std::vector<uint8_t> out = transcode(std::move(packet));
      if (out.empty()) continue;
      const std::size_t decoded = out.size();
      bool announce = false;
      if (av1) {
        const auto now = std::chrono::steady_clock::now();
        if (last_stat_.time_since_epoch().count() == 0 ||
            now - last_stat_ >= std::chrono::seconds(1)) {
          last_stat_ = now;
          announce = true;
        }
      }
      std::weak_ptr<direct_proxy> weak = weak_from_this();
      std::vector<uint8_t> bytes = std::move(out);
      post_([weak, bytes = std::move(bytes), announce, wire, decoded] {
        if (auto self = weak.lock()) {
          if (announce) self->send_codec_stat(wire, decoded);
          self->send_to_browser(bytes.data(), bytes.size());
        }
      });
    }
  }

  void send_to_browser(const uint8_t* data, std::size_t len) {
    if (closed_ || browser_ == nullptr) return;
    browser_->send_binary(reinterpret_cast<const char*>(data), len);
  }

  void send_codec_stat(std::size_t wire, std::size_t decoded) {
    if (closed_ || browser_ == nullptr) return;
    const std::string text = "{\"type\":\"codec\",\"av1\":true,\"wire\":" +
                             std::to_string(wire) + ",\"decoded\":" +
                             std::to_string(decoded) + "}";
    browser_->send_text(text.data(), text.size());
  }

  std::vector<uint8_t> transcode(std::vector<uint8_t> bytes) {
    protocol::packet packet;
    std::string error;
    if (!protocol::decode_packet(bytes.data(), bytes.size(), packet, error)) {
      return bytes;
    }
    if (packet.type != static_cast<uint8_t>(protocol::packet_type::frame) ||
        packet.payload.size() < protocol::kFrameHeaderSize ||
        packet.payload[6] != static_cast<uint8_t>(protocol::codec::av1)) {
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
  std::function<void(std::function<void()>)> post_;
  std::shared_ptr<relay_transport> transport_;
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
};

}  // namespace

uvcpp::uvcpp_web_ws_handler make_direct_handler(
    std::function<void(std::function<void()>)> post) {
  return [post = std::move(post)](uvcpp::uvcpp_web_ws_request& request) {
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
    auto proxy = std::make_shared<direct_proxy>(conn, *host, port_number, post);
    proxy->start();
  };
}

}  // namespace molink
