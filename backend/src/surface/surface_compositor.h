#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "protocol/frame_packet.h"

namespace molink {

class av1_stream_decoder;

/// Composes the picture a viewer core receives into one BGRA canvas and
/// publishes it to the native frame surface, so the shell can draw the remote
/// screen without the WebView in the path.
///
/// Tile codecs (raw and deflated BGRA/RGB24/RGB565/gray8) are composed
/// directly; the low-delay AV1 and VP9/VP8 streams are decoded in the core
/// (dav1d / libvpx) and published as whole frames. H.264 needs a hardware
/// encoder/decoder and stays off this path.
class surface_compositor {
 public:
  surface_compositor();
  ~surface_compositor();
  surface_compositor(const surface_compositor&) = delete;
  surface_compositor& operator=(const surface_compositor&) = delete;

  /// Integrates one parsed frame. Returns true when the canvas was published.
  /// Runs on the decode thread (or synchronously when no packet was queued).
  bool ingest(const protocol::parsed_frame& frame);

  /// Queues one serialized FRAME packet for the decode thread. The network
  /// loop must not spend 10-30 ms decoding a 1080p stream: that stalls every
  /// other transport and shows up as jitter. Returns false when the native
  /// surface is off or the packet is not a frame.
  bool enqueue_packet(const uint8_t* data, std::size_t len);

  /// Drops the canvas (a resolution change or a new session).
  void reset();

  int width() const { return width_; }
  int height() const { return height_; }
  uint64_t published() const { return published_; }

 private:
  struct vpx_decoder;

  bool ensure(int width, int height);
  void reset_sync();
  void worker_loop();
  bool ingest_av1_stream(const protocol::parsed_frame& frame, bool keyframe);
  bool ingest_vpx_stream(const protocol::parsed_frame& frame,
                         protocol::codec value, bool keyframe);
  bool publish_bgra(const std::vector<uint8_t>& bgra, int width, int height);

  std::vector<uint8_t> canvas_;
  int width_ = 0;
  int height_ = 0;
  uint64_t published_ = 0;
  /// Scratch buffer for deflated tiles, reused across frames.
  std::vector<uint8_t> inflated_;
  /// Low-delay stream decoders, kept across frames (deltas reference the
  /// previous picture).
  std::unique_ptr<av1_stream_decoder> av1_;
  std::unique_ptr<vpx_decoder> vpx_;
  int vpx_codec_kind_ = 0;
  bool stream_has_image_ = false;
  // Decode thread: packets arrive on the network loop and are decoded here in
  // order, so a slow stream decode never blocks the transport.
  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<std::vector<uint8_t>> queue_;
  std::size_t queued_bytes_ = 0;
  bool worker_started_ = false;
  bool stop_ = false;
  std::atomic<bool> reset_pending_{false};
  /// Queue overflow or a failed stream decode: skip deltas until a keyframe.
  std::atomic<bool> need_keyframe_{false};
};

/// Feed one serialized packet to the compositor. Returns true when a picture
/// was published. A cheap no-op for control packets and while the native
/// surface is off.
bool surface_ingest_packet(surface_compositor& compositor,
                           const uint8_t* data, std::size_t len);

}  // namespace molink
