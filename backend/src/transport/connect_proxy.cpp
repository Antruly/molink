#include "surface/surface_compositor.h"
#include "transport/connect_proxy.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/av1_frame.h"
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
    browser_->set_max_message_size(16u * 1024u * 1024u);
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
    probe_enabled_ = std::getenv("MOLINK_AV1_PROBE") != nullptr;
    start_worker();
    if (probe_enabled_ && !worker_started_) {
      std::fprintf(stderr, "proxy: av1 probe off (no dav1d worker)\n");
    }
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
    // The page cannot reach a console here; it tunnels decoder failures over
    // this channel so the core's log can carry the WebKit error text.
    if (text.find("\"client-log\"") != std::string::npos) {
      std::fprintf(stderr, "proxy: %s\n", text.c_str());
    }
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
    note_frame_for_stats(data, len);
    wire_window_.fetch_add(len);
    // Forwarded on the loop thread: transcode() is a passthrough now, so the
    // worker only added a thread hop -- and its 8-packet queue dropped the
    // oldest packet under a burst. A dropped stream frame breaks the reference
    // chain and the picture smears until a keyframe, which is exactly the
    // intermittent "VP9 corruption" that was reported.
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

  /// Counts frames by codec and prints one line a second. This is what tells
  /// "the host's stream never reaches the proxy" apart from "the page receives
  /// it and does not paint it".
  void note_frame_for_stats(const uint8_t* data, std::size_t len) {
    if (len >= protocol::kPacketHeaderSize + protocol::kFrameHeaderSize &&
        data[5] == static_cast<uint8_t>(protocol::packet_type::frame)) {
      ++frame_codec_counts_[data[protocol::kPacketHeaderSize + 6]];
      ++frame_count_;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_frame_log_.time_since_epoch().count() != 0 &&
        now - last_frame_log_ < std::chrono::seconds(1)) {
      return;
    }
    last_frame_log_ = now;
    if (frame_count_ == 0) return;
    if (std::getenv("MOLINK_STREAM_LOG") == nullptr) {
      frame_codec_counts_.clear();
      frame_count_ = 0;
      return;
    }
    std::string line = "proxy: frames received";
    for (const auto& [codec, count] : frame_codec_counts_) {
      line += " codec" + std::to_string(codec) + "=" + std::to_string(count);
    }
    std::fprintf(stderr, "%s\n", line.c_str());
    frame_codec_counts_.clear();
    frame_count_ = 0;
  }

  /// One codec stat per second for the page's wire/decoded readout.
  void maybe_announce_codec(const uint8_t* data, std::size_t len) {
    if (closed_ || browser_ == nullptr) return;
    // Header-only check: copying a whole frame just to read six bytes was
    // pure overhead on the loop thread.
    if (len < protocol::kPacketHeaderSize + protocol::kFrameHeaderSize) return;
    if (std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) != 0) {
      return;
    }
    if (data[5] != static_cast<uint8_t>(protocol::packet_type::frame)) return;
    const uint8_t codec = data[protocol::kPacketHeaderSize + 6];
    if (codec != static_cast<uint8_t>(protocol::codec::av1) &&
        codec != static_cast<uint8_t>(protocol::codec::av1_stream)) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_stat_.time_since_epoch().count() != 0 &&
        now - last_stat_ < std::chrono::seconds(1)) {
      return;
    }
    const double seconds =
        window_start_.time_since_epoch().count() == 0
            ? 0.0
            : std::chrono::duration<double>(now - window_start_).count();
    const std::size_t bytes = wire_window_.exchange(0);
    std::size_t wire_rate = 0;
    if (seconds > 0.2) {
      wire_rate = static_cast<std::size_t>(bytes / seconds);
    }
    window_start_ = now;
    last_stat_ = now;
    const bool full_frame = len > 6 && (data[6] & protocol::kPacketFlagTileRefresh) == 0;
    send_codec_stat(len, len, full_frame, wire_rate);
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
      if (av1 && probe_enabled_) probe_av1_frame(packet);
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

  /// Diagnostic (MOLINK_AV1_PROBE=1): decode the host's AV1 stream with the
  /// core's own dav1d. A valid picture here with the page still failing means
  /// the WebView's WebCodecs decoder is the problem, not the bitstream.
  void probe_av1_frame(const std::vector<uint8_t>& packet) {
    protocol::parsed_frame parsed;
    std::string parse_error;
    if (!protocol::parse_frame_payload(
            packet.data() + protocol::kPacketHeaderSize,
            packet.size() - protocol::kPacketHeaderSize, parsed,
            parse_error) ||
        parsed.tiles.empty()) {
      std::fprintf(stderr, "proxy: av1 probe parse failed: %s\n",
                   parse_error.c_str());
      return;
    }
    const bool keyframe = (packet[6] & protocol::kPacketFlagKeyframe) != 0;
    if (keyframe && !probe_dumped_) {
      probe_dumped_ = true;
      if (FILE* file = std::fopen("/tmp/molink-av1-key.obu", "wb")) {
        std::fwrite(parsed.tiles[0].data, 1, parsed.tiles[0].size, file);
        std::fclose(file);
        std::fprintf(stderr, "proxy: av1 probe dumped keyframe (%zu bytes)\n",
                     parsed.tiles[0].size);
      }
    }
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    std::string error;
    const bool ok = stream_decoder_.decode(
        parsed.tiles[0].data, parsed.tiles[0].size, keyframe, bgra, width,
        height, error);
    std::fprintf(stderr, "proxy: av1 probe key=%d size=%zu ok=%d %dx%d%s%s\n",
                 keyframe ? 1 : 0, parsed.tiles[0].size, ok ? 1 : 0, width,
                 height, ok ? "" : " error=", ok ? "" : error.c_str());
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
    // The native surface mirrors the picture the page receives (tile codecs
    // only for now; the streams wait for the core-side decoder).
    surface_ingest_packet(compositor_, data, len);
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
  /// MOLINK_AV1_PROBE=1: decode the incoming AV1 stream for diagnostics only.
  bool probe_enabled_ = false;
  bool probe_dumped_ = false;
  std::chrono::steady_clock::time_point last_stat_{};
  /// The low-delay stream keeps reference frames per session.
  av1_stream_decoder stream_decoder_;
  surface_compositor compositor_;
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
  /// Frame counts per codec since the last diagnostic line.
  std::map<uint8_t, int> frame_codec_counts_;
  int frame_count_ = 0;
  std::chrono::steady_clock::time_point last_frame_log_{};
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
