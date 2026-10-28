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
    // Advertising AV1 without the worker would forward a bitstream the page
    // cannot decode, so both the decoder and the loop post are required.
    if (!av1_decode_available() || !post_) return text;
    try {
      nlohmann::json hello = nlohmann::json::parse(text);
      if (!hello.is_object()) return text;
      hello["av1"] = true;
      hello["av1_stream"] = true;
      if (std::getenv("MOLINK_AV1_LOG") != nullptr) {
        std::fprintf(stderr, "proxy: injected av1 into hello\n");
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
      if (!stream_decoder_.decode(view.data, view.size, keyframe, bgra, width,
                                  height, error)) {
        std::fprintf(stderr, "connect proxy: av1 stream decode failed: %s\n",
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
          std::weak_ptr<connect_proxy> weak_self = weak_from_this();
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
      // JPEG: a 1080p desktop frame is a few hundred kilobytes and the page
      // decodes it with the browser's native image decoder. Raw RGB565 (four
      // megabytes per frame here) hit the WebSocket's throughput ceiling.
      const int quality = 80;
      bool packed_jpeg = false;
      if (jpeg_encode_available()) {
        std::string jpeg_error;
        std::vector<uint8_t> jpeg;
        if (jpeg_encode_bgra(bgra.data(), static_cast<std::size_t>(width) * 4,
                             width, height, quality, jpeg, jpeg_error)) {
          tile.data = std::move(jpeg);
          header.codec = static_cast<uint8_t>(protocol::codec::jpeg);
          packed_jpeg = true;
        } else if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
          std::fprintf(stderr, "%s: jpeg encode failed: %s\n", "connect proxy",
                       jpeg_error.c_str());
        }
      }
      if (!packed_jpeg) {
        // Fallback: raw RGB565, half the bytes of BGRA.
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
      }
      std::vector<protocol::tile_blob> tiles;
      tiles.push_back(std::move(tile));
      // No deflate: a full-screen frame compresses to ~1.5 MB, which costs
      // ~43 ms on the core and ~15 ms to inflate in the page, while raw
      // RGB565 is 4.1 MB that loops back in a couple of milliseconds.
      std::vector<uint8_t> payload;
      if (!protocol::build_frame_payload(header, tiles, payload, error)) {
        return {};
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
      std::fprintf(stderr, "proxy: av1 decode failed: %s\n", error.c_str());
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
    if (std::getenv("MOLINK_AV1_LOG") != nullptr) {
      std::fprintf(stderr, "proxy: av1 %zu -> %zu bytes, %zu tiles\n",
                   bytes.size(), out.size(), tiles.size());
    }
    return out;
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
