#include "surface/surface_compositor.h"

#include <algorithm>
#include <cstring>

#include <cstring>

#include "surface/frame_surface.h"

#if defined(MOLINK_HAVE_ZLIB) && MOLINK_HAVE_ZLIB
#include <zlib.h>
#endif

namespace molink {
namespace {

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

void surface_compositor::reset() {
  canvas_.clear();
  width_ = 0;
  height_ = 0;
  published_ = 0;
  inflated_.clear();
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
  if (!is_tile_codec(value)) return false;  // streams wait for stage 2
  if (!ensure(frame.header.screen_w, frame.header.screen_h)) return false;

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
  if (!frame_surface::instance().active()) return false;
  if (len < protocol::kPacketHeaderSize + protocol::kFrameHeaderSize) {
    return false;
  }
  if (std::memcmp(data, protocol::kMagic, sizeof(protocol::kMagic)) != 0) {
    return false;
  }
  if (data[5] != static_cast<uint8_t>(protocol::packet_type::frame)) {
    return false;
  }
  protocol::parsed_frame parsed;
  std::string error;
  if (!protocol::parse_frame_payload(data + protocol::kPacketHeaderSize,
                                     len - protocol::kPacketHeaderSize, parsed,
                                     error)) {
    return false;
  }
  return compositor.ingest(parsed);
}

}  // namespace molink
