#include "surface/surface_compositor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cstring>

#include "capture/av1_frame.h"
#include "surface/frame_surface.h"

#if defined(MOLINK_HAVE_ZLIB) && MOLINK_HAVE_ZLIB
#include <zlib.h>
#endif

#if defined(MOLINK_HAVE_VPX) && MOLINK_HAVE_VPX
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#endif

namespace molink {
namespace {

/// Rate-limited diagnostics for the native path (MOLINK_SURFACE_LOG=1).
bool surface_log_due() {
  static uint64_t last = 0;
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  if (now - last < 1000) return false;
  last = now;
  return true;
}

using protocol::codec;

/// True for the self-contained pixel codecs this compositor understands.
bool is_tile_codec(codec value) {
  switch (value) {
    case codec::bgra:
    case codec::rgb24:
    case codec::rgb565:
    case codec::gray8:
    case codec::deflate_bgra:
    case codec::deflate_rgb24:
    case codec::deflate_rgb565:
    case codec::deflate_gray8:
      return true;
    default:
      return false;
  }
}

codec base_codec(codec value) {
  switch (value) {
    case codec::deflate_bgra:
      return codec::bgra;
    case codec::deflate_rgb24:
      return codec::rgb24;
    case codec::deflate_rgb565:
      return codec::rgb565;
    case codec::deflate_gray8:
      return codec::gray8;
    default:
      return value;
  }
}

std::size_t bytes_per_pixel(codec value) {
  switch (value) {
    case codec::bgra:
      return 4;
    case codec::rgb24:
      return 3;
    case codec::rgb565:
      return 2;
    case codec::gray8:
      return 1;
    default:
      return 0;
  }
}

/// Copies one tile into the canvas, converting to BGRA and clipping to the
/// canvas bounds. `source` is tightly packed for `codec`.
void blit(const uint8_t* source, codec value, int tile_w, int tile_h, int x,
          int y, uint8_t* canvas, int canvas_w, int canvas_h) {
  const int x0 = (std::max)(0, x);
  const int y0 = (std::max)(0, y);
  const int x1 = (std::min)(canvas_w, x + tile_w);
  const int y1 = (std::min)(canvas_h, y + tile_h);
  if (x1 <= x0 || y1 <= y0) return;
  const std::size_t bpp = bytes_per_pixel(value);
  if (bpp == 0) return;
  for (int row = y0; row < y1; ++row) {
    const uint8_t* src = source +
                         (static_cast<std::size_t>(row - y) * tile_w +
                          static_cast<std::size_t>(x0 - x)) *
                             bpp;
    uint8_t* dst = canvas + (static_cast<std::size_t>(row) * canvas_w + x0) * 4;
    const int count = x1 - x0;
    switch (value) {
      case codec::bgra:
        std::memcpy(dst, src, static_cast<std::size_t>(count) * 4);
        break;
      case codec::rgb24:
        for (int i = 0; i < count; ++i) {
          dst[i * 4 + 0] = src[i * 3 + 2];
          dst[i * 4 + 1] = src[i * 3 + 1];
          dst[i * 4 + 2] = src[i * 3 + 0];
          dst[i * 4 + 3] = 255;
        }
        break;
      case codec::rgb565:
        for (int i = 0; i < count; ++i) {
          const uint16_t pixel =
              static_cast<uint16_t>(src[i * 2] | (src[i * 2 + 1] << 8));
          const uint8_t r = static_cast<uint8_t>(((pixel >> 11) & 0x1f) * 255 / 31);
          const uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x3f) * 255 / 63);
          const uint8_t b = static_cast<uint8_t>((pixel & 0x1f) * 255 / 31);
          dst[i * 4 + 0] = b;
          dst[i * 4 + 1] = g;
          dst[i * 4 + 2] = r;
          dst[i * 4 + 3] = 255;
        }
        break;
      case codec::gray8:
        for (int i = 0; i < count; ++i) {
          const uint8_t luma = src[i];
          dst[i * 4 + 0] = luma;
          dst[i * 4 + 1] = luma;
          dst[i * 4 + 2] = luma;
          dst[i * 4 + 3] = 255;
        }
        break;
      default:
        break;
    }
  }
}

}  // namespace

#if defined(MOLINK_HAVE_VPX) && MOLINK_HAVE_VPX
struct surface_compositor::vpx_decoder {
  vpx_codec_ctx_t ctx{};
  bool open = false;
  ~vpx_decoder() {
    if (open) vpx_codec_destroy(&ctx);
  }
  bool init(bool vp9) {
    if (open) {
      vpx_codec_destroy(&ctx);
      open = false;
    }
    const vpx_codec_iface_t* iface =
        vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();
    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = 4;  // tile/row parallel decode
    if (vpx_codec_dec_init(&ctx, iface, &cfg, 0) != VPX_CODEC_OK) {
      return false;
    }
    open = true;
    return true;
  }
};
#else
struct surface_compositor::vpx_decoder {};
#endif

surface_compositor::surface_compositor() = default;

surface_compositor::~surface_compositor() {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    stop_ = true;
    queue_.clear();
    queued_bytes_ = 0;
  }
  queue_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void surface_compositor::reset() {
  bool started = false;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    started = worker_started_;
    if (started) {
      // Stale frames from the old session must not decode into the new one.
      queue_.clear();
      queued_bytes_ = 0;
      reset_pending_.store(true);
    }
  }
  if (started) {
    queue_cv_.notify_all();
  } else {
    reset_sync();
  }
}

void surface_compositor::reset_sync() {
  canvas_.clear();
  width_ = 0;
  height_ = 0;
  published_ = 0;
  inflated_.clear();
  av1_.reset();
  vpx_.reset();
  vpx_codec_kind_ = 0;
  stream_has_image_ = false;
  need_keyframe_.store(false);
}

bool surface_compositor::enqueue_packet(const uint8_t* data, std::size_t len) {
  if (!frame_surface::instance().active() || data == nullptr) return false;
  if (len < protocol::kPacketHeaderSize + protocol::kFrameHeaderSize) {
    return false;
  }
  if (std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) != 0) {
    return false;
  }
  if (data[5] != static_cast<uint8_t>(protocol::packet_type::frame)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!worker_started_) {
      worker_ = std::thread([this] { worker_loop(); });
      worker_started_ = true;
    }
    // Bound memory and latency. Dropping an individual delta corrupts the
    // reference chain, so on overflow drop the backlog and wait for the next
    // keyframe instead.
    constexpr std::size_t kMaxQueuedPackets = 512;
    constexpr std::size_t kMaxQueuedBytes = 96u * 1024u * 1024u;
    if (queue_.size() >= kMaxQueuedPackets ||
        queued_bytes_ + len > kMaxQueuedBytes) {
      queue_.clear();
      queued_bytes_ = 0;
      need_keyframe_.store(true);
    }
    queue_.emplace_back(data, data + len);
    queued_bytes_ += len;
  }
  queue_cv_.notify_one();
  return true;
}

void surface_compositor::worker_loop() {
  for (;;) {
    std::vector<uint8_t> packet;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      packet = std::move(queue_.front());
      queue_.pop_front();
      queued_bytes_ -= packet.size();
    }
    if (reset_pending_.exchange(false)) reset_sync();
    protocol::parsed_frame parsed;
    std::string error;
    if (!protocol::parse_frame_payload(
            packet.data() + protocol::kPacketHeaderSize,
            packet.size() - protocol::kPacketHeaderSize, parsed, error)) {
      continue;
    }
    const protocol::codec kind =
        static_cast<protocol::codec>(parsed.header.codec);
    const bool stream =
        kind == protocol::codec::av1_stream ||
        kind == protocol::codec::h264_stream ||
        kind == protocol::codec::vp9_stream ||
        kind == protocol::codec::vp8_stream;
    const bool keyframe =
        (parsed.header.flags & protocol::kFrameFlagKeyframe) != 0;
    if (stream && need_keyframe_.load() && !keyframe) continue;
    if (keyframe) need_keyframe_.store(false);
    const bool published = ingest(parsed);
    if (!published && stream) need_keyframe_.store(true);
    if (std::getenv("MOLINK_SURFACE_LOG") != nullptr && surface_log_due()) {
      std::fprintf(stderr,
                   "surface: packet=%zu codec=%u tiles=%u %ux%u key=%d "
                   "published=%d%s%s\n",
                   packet.size(), parsed.header.codec,
                   parsed.header.tile_count, parsed.header.screen_w,
                   parsed.header.screen_h, keyframe ? 1 : 0, published ? 1 : 0,
                   error.empty() ? "" : " err=", error.c_str());
    }
  }
}

bool surface_compositor::publish_bgra(const std::vector<uint8_t>& bgra,
                                      int width, int height) {
  if (width <= 0 || height <= 0 ||
      bgra.size() < static_cast<std::size_t>(width) * height * 4) {
    return false;
  }
  frame_surface::instance().publish(bgra.data(), width, height, width * 4,
                                    frame_surface::format::bgra);
  width_ = width;
  height_ = height;
  ++published_;
  return true;
}

bool surface_compositor::ingest_av1_stream(
    const protocol::parsed_frame& frame, bool keyframe) {
  if (frame.tiles.empty() || frame.tiles[0].data == nullptr ||
      frame.tiles[0].size == 0) {
    return false;
  }
  // A delta references the previous picture; wait for a keyframe.
  if (!stream_has_image_ && !keyframe) return false;
  if (!av1_) av1_ = std::make_unique<av1_stream_decoder>();
  std::vector<uint8_t> bgra;
  int width = 0;
  int height = 0;
  std::string error;
  if (!av1_->decode(frame.tiles[0].data, frame.tiles[0].size, keyframe, bgra,
                    width, height, error)) {
    if (std::getenv("MOLINK_SURFACE_LOG") != nullptr && surface_log_due()) {
      std::fprintf(stderr, "surface: av1 stream decode failed: %s\n",
                   error.c_str());
    }
    av1_->reset();
    stream_has_image_ = false;
    return false;
  }
  stream_has_image_ = true;
  return publish_bgra(bgra, width, height);
}

bool surface_compositor::ingest_vpx_stream(
    const protocol::parsed_frame& frame, protocol::codec value, bool keyframe) {
#if defined(MOLINK_HAVE_VPX) && MOLINK_HAVE_VPX
  if (frame.tiles.empty() || frame.tiles[0].data == nullptr ||
      frame.tiles[0].size == 0) {
    return false;
  }
  if (!stream_has_image_ && !keyframe) return false;
  const bool vp9 = value == codec::vp9_stream;
  const int kind = vp9 ? 9 : 8;
  if (!vpx_ || vpx_codec_kind_ != kind) {
    vpx_ = std::make_unique<vpx_decoder>();
    if (!vpx_->init(vp9)) {
      vpx_.reset();
      return false;
    }
    vpx_codec_kind_ = kind;
  }
  if (vpx_codec_decode(&vpx_->ctx, frame.tiles[0].data,
                       static_cast<unsigned int>(frame.tiles[0].size), nullptr,
                       0) != VPX_CODEC_OK) {
    vpx_.reset();
    stream_has_image_ = false;
    return false;
  }
  vpx_codec_iter_t iterator = nullptr;
  vpx_image_t* image = vpx_codec_get_frame(&vpx_->ctx, &iterator);
  if (image == nullptr || image->fmt != VPX_IMG_FMT_I420) return false;
  const int width = static_cast<int>(image->d_w);
  const int height = static_cast<int>(image->d_h);
  if (width <= 0 || height <= 0) return false;
  std::vector<uint8_t> bgra(static_cast<std::size_t>(width) * height * 4, 0);
  // Honour the bitstream's color range: libvpx writes studio-range samples
  // for the web and signals it, so treating them as full range washes the
  // picture out next to the browser canvas.
  const bool full_range = image->range == VPX_CR_FULL_RANGE;
  // The matrix travels with the bitstream; libvpx's screen encoder writes
  // BT.601 weights, AV1/SVT writes BT.709.
  const bool bt709 = image->cs == VPX_CS_BT_709;
  const int b_coef = bt709 ? 475 : 454;   // 1.8556 / 1.772
  const int r_coef = bt709 ? 403 : 359;   // 1.5748 / 1.402
  const int g_cb = bt709 ? 48 : 88;       // 0.1873 / 0.3441
  const int g_cr = bt709 ? 120 : 183;     // 0.4681 / 0.7141
  for (int y = 0; y < height; ++y) {
    const uint8_t* y_row = image->planes[VPX_PLANE_Y] +
                           static_cast<std::ptrdiff_t>(y) * image->stride[VPX_PLANE_Y];
    const uint8_t* u_row =
        image->planes[VPX_PLANE_U] +
        static_cast<std::ptrdiff_t>(y / 2) * image->stride[VPX_PLANE_U];
    const uint8_t* v_row =
        image->planes[VPX_PLANE_V] +
        static_cast<std::ptrdiff_t>(y / 2) * image->stride[VPX_PLANE_V];
    uint8_t* out = bgra.data() + static_cast<std::size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x) {
      int luma = y_row[x];
      int cb = u_row[x / 2] - 128;
      int cr = v_row[x / 2] - 128;
      if (!full_range) {
        luma = ((luma - 16) * 255 + 109) / 219;
        cb = (cb * 255 + 112) / 224;
        cr = (cr * 255 + 112) / 224;
      }
      out[x * 4 + 0] = static_cast<uint8_t>(
          std::clamp(luma + ((b_coef * cb) >> 8), 0, 255));
      out[x * 4 + 1] = static_cast<uint8_t>(std::clamp(
          luma - ((g_cb * cb + g_cr * cr) >> 8), 0, 255));
      out[x * 4 + 2] = static_cast<uint8_t>(
          std::clamp(luma + ((r_coef * cr) >> 8), 0, 255));
      out[x * 4 + 3] = 255;
    }
  }
  stream_has_image_ = true;
  return publish_bgra(bgra, width, height);
#else
  (void)frame;
  (void)value;
  (void)keyframe;
  return false;
#endif
}

bool surface_compositor::ensure(int width, int height) {
  if (width <= 0 || height <= 0) return false;
  if (width == width_ && height == height_ && !canvas_.empty()) return true;
  width_ = width;
  height_ = height;
  canvas_.assign(static_cast<std::size_t>(width) * height * 4, 0);
  return true;
}

bool surface_compositor::ingest(const protocol::parsed_frame& frame) {
  if (!frame_surface::instance().active()) return false;
  const codec value = static_cast<codec>(frame.header.codec);
  const bool keyframe =
      (frame.header.flags & protocol::kFrameFlagKeyframe) != 0;
  if (value == codec::av1_stream) {
    return ingest_av1_stream(frame, keyframe);
  }
  if (value == codec::vp9_stream || value == codec::vp8_stream) {
    return ingest_vpx_stream(frame, value, keyframe);
  }
  if (!is_tile_codec(value)) {
    if (std::getenv("MOLINK_SURFACE_LOG") != nullptr && surface_log_due()) {
      std::fprintf(stderr, "surface: stream codec %u skipped (%u tiles)\n",
                   static_cast<unsigned>(frame.header.codec),
                   frame.header.tile_count);
    }
    return false;
  }
  if (!ensure(frame.header.screen_w, frame.header.screen_h)) {
    if (std::getenv("MOLINK_SURFACE_LOG") != nullptr && surface_log_due()) {
      std::fprintf(stderr, "surface: bad size %ux%u\n",
                   frame.header.screen_w, frame.header.screen_h);
    }
    return false;
  }

  bool painted = false;
  for (const protocol::tile_view& tile : frame.tiles) {
    if (tile.data == nullptr || tile.w == 0 || tile.h == 0) continue;
    const codec base = base_codec(value);
    const std::size_t bpp = bytes_per_pixel(base);
    if (bpp == 0) continue;
    const std::size_t expected =
        static_cast<std::size_t>(tile.w) * tile.h * bpp;
    const uint8_t* source = tile.data;

    if (value != base) {
#if defined(MOLINK_HAVE_ZLIB) && MOLINK_HAVE_ZLIB
      inflated_.resize(expected);
      uLongf out = static_cast<uLongf>(expected);
      if (uncompress(inflated_.data(), &out, tile.data,
                     static_cast<uLong>(tile.size)) != Z_OK ||
          out != expected) {
        continue;
      }
      source = inflated_.data();
#else
      continue;  // deflated tiles need zlib, which this build lacks
#endif
    } else if (tile.size < expected) {
      continue;
    }
    blit(source, base, tile.w, tile.h, tile.x, tile.y, canvas_.data(), width_,
         height_);
    painted = true;
  }

  if (!painted) return false;
  frame_surface::instance().publish(canvas_.data(), width_, height_,
                                    width_ * 4, frame_surface::format::bgra);
  ++published_;
  return true;
}

bool surface_ingest_packet(surface_compositor& compositor,
                           const uint8_t* data, std::size_t len) {
  // The network loop only queues; parsing, decoding and publishing happen on
  // the compositor's decode thread.
  return compositor.enqueue_packet(data, len);
}

}  // namespace molink
