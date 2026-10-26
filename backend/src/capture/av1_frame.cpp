#include "capture/av1_frame.h"

#include <algorithm>
#include <cstring>

#include "capture/av1_encoder.h"

namespace molink {

bool build_av1_frame_payload(const protocol::frame_header& header,
                             av1_encoder& encoder, const uint8_t* bgra,
                             std::size_t stride,
                             const std::vector<pixel_rect>& rects, int qp,
                             std::vector<uint8_t>& payload,
                             std::string& error, bool grayscale) {
  payload.clear();
  if (bgra == nullptr || rects.empty()) return false;
  std::vector<protocol::tile_blob> tiles;
  for (const pixel_rect& rect : rects) {
    if (rect.w <= 0 || rect.h <= 0) continue;
    const uint8_t* source =
        bgra + static_cast<std::size_t>(rect.y) * stride +
        static_cast<std::size_t>(rect.x) * 4;
    std::size_t source_stride = stride;
    std::vector<uint8_t> gray;
    if (grayscale) {
      // The turbo preset is monochrome: give AV1 luminance-only pixels so the
      // rectangles keep the preset's look.
      gray.resize(static_cast<std::size_t>(rect.w) * rect.h * 4);
      for (int row = 0; row < rect.h; ++row) {
        const uint8_t* in =
            source + static_cast<std::size_t>(row) * stride;
        uint8_t* out =
            gray.data() + static_cast<std::size_t>(row) * rect.w * 4;
        for (int col = 0; col < rect.w; ++col) {
          const uint8_t luma = static_cast<uint8_t>(
              (in[col * 4 + 2] * 77 + in[col * 4 + 1] * 150 +
               in[col * 4] * 29) >> 8);
          out[col * 4 + 0] = luma;
          out[col * 4 + 1] = luma;
          out[col * 4 + 2] = luma;
          out[col * 4 + 3] = 255;
        }
      }
      source = gray.data();
      source_stride = static_cast<std::size_t>(rect.w) * 4;
    }
    protocol::tile_blob tile;
    tile.x = static_cast<uint16_t>(rect.x);
    tile.y = static_cast<uint16_t>(rect.y);
    tile.w = static_cast<uint16_t>(rect.w);
    tile.h = static_cast<uint16_t>(rect.h);
    tile.flags = protocol::kTileFlagChanged;
    if (!encoder.encode_intra(source, source_stride, rect.w, rect.h, qp,
                              tile.data, error)) {
      return false;
    }
    if (tile.data.empty()) continue;
    tiles.push_back(std::move(tile));
  }
  if (tiles.empty()) {
    error = "av1: no rectangle produced a bitstream";
    return false;
  }
  protocol::frame_header out = header;
  out.codec = static_cast<uint8_t>(protocol::codec::av1);
  // Keep the keyframe flag: a full-frame AV1 picture is the only packet the
  // session sends, so the receiver has to know it can start from it. Patch
  // frames arrive with the partial flag, exactly as before.
  out.flags = static_cast<uint8_t>(header.flags & protocol::kFrameFlagKeyframe);
  return protocol::build_frame_payload(out, tiles, payload, error);
}

bool av1_decode_available() {
#if defined(MOLINK_HAVE_DAV1D)
  return true;
#else
  return false;
#endif
}

#if defined(MOLINK_HAVE_DAV1D)

#include <cerrno>
#include <dav1d/dav1d.h>

namespace {

uint8_t clamp_u8(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

/// Inverse of the encoder's full-range BT.709 conversion.
inline void yuv_to_bgra(int y, int u, int v, uint8_t* out) {
  const int cb = u - 128;
  const int cr = v - 128;
  out[0] = clamp_u8(y + ((475 * cb) >> 8));
  out[1] = clamp_u8(y - ((48 * cb + 120 * cr) >> 8));
  out[2] = clamp_u8(y + ((403 * cr) >> 8));
  out[3] = 255;
}

bool decode_tile(const protocol::tile_view& tile, protocol::tile_blob& out,
                 std::string& error) {
  Dav1dSettings settings{};
  dav1d_default_settings(&settings);
  settings.n_threads = 1;
  settings.max_frame_delay = 1;
  Dav1dContext* ctx = nullptr;
  if (dav1d_open(&ctx, &settings) != 0) {
    error = "av1: could not open dav1d";
    return false;
  }
  Dav1dData data{};
  uint8_t* dst = dav1d_data_create(&data, tile.size);
  if (dst == nullptr) {
    dav1d_close(&ctx);
    error = "av1: could not allocate the dav1d input";
    return false;
  }
  std::memcpy(dst, tile.data, tile.size);

  int rc = dav1d_send_data(ctx, &data);
  dav1d_data_unref(&data);
  if (rc < 0 && rc != DAV1D_ERR(EAGAIN)) {
    dav1d_close(&ctx);
    error = "av1: dav1d rejected the bitstream";
    return false;
  }
  Dav1dPicture picture{};
  rc = dav1d_get_picture(ctx, &picture);
  if (rc != 0) {
    dav1d_close(&ctx);
    error = "av1: dav1d produced no picture";
    return false;
  }
  if (picture.p.layout != DAV1D_PIXEL_LAYOUT_I420) {
    dav1d_picture_unref(&picture);
    dav1d_close(&ctx);
    error = "av1: unexpected pixel layout";
    return false;
  }
  const int width = std::min<int>(tile.w, picture.p.w);
  const int height = std::min<int>(tile.h, picture.p.h);
  out.x = tile.x;
  out.y = tile.y;
  out.w = tile.w;
  out.h = tile.h;
  out.flags = protocol::kTileFlagChanged;
  out.data.resize(static_cast<std::size_t>(tile.w) * tile.h * 4, 0);
  const auto* luma = static_cast<const uint8_t*>(picture.data[0]);
  const auto* chroma_u = static_cast<const uint8_t*>(picture.data[1]);
  const auto* chroma_v = static_cast<const uint8_t*>(picture.data[2]);
  const int luma_stride = picture.stride[0];
  const int chroma_stride = picture.stride[1];
  for (int y = 0; y < height; ++y) {
    const uint8_t* y_row = luma + static_cast<std::size_t>(y) * luma_stride;
    const uint8_t* u_row =
        chroma_u + static_cast<std::size_t>(y / 2) * chroma_stride;
    const uint8_t* v_row =
        chroma_v + static_cast<std::size_t>(y / 2) * chroma_stride;
    uint8_t* out_row =
        out.data.data() + static_cast<std::size_t>(y) * tile.w * 4;
    for (int x = 0; x < width; ++x) {
      yuv_to_bgra(y_row[x], u_row[x / 2], v_row[x / 2],
                  out_row + static_cast<std::size_t>(x) * 4);
    }
  }
  dav1d_picture_unref(&picture);
  dav1d_close(&ctx);
  return true;
}

}  // namespace

bool decode_av1_frame_payload(const uint8_t* payload, std::size_t len,
                              std::vector<protocol::tile_blob>& tiles,
                              std::string& error) {
  tiles.clear();
  protocol::parsed_frame frame;
  if (!protocol::parse_frame_payload(payload, len, frame, error)) return false;
  if (frame.header.codec != static_cast<uint8_t>(protocol::codec::av1)) {
    error = "av1: frame is not AV1";
    return false;
  }
  std::vector<protocol::tile_blob> decoded;
  decoded.reserve(frame.tiles.size());
  for (const protocol::tile_view& tile : frame.tiles) {
    protocol::tile_blob out;
    std::string tile_error;
    if (!decode_tile(tile, out, tile_error)) {
      error = tile_error;
      return false;
    }
    decoded.push_back(std::move(out));
  }
  tiles = std::move(decoded);
  return true;
}

#else

bool decode_av1_frame_payload(const uint8_t* payload, std::size_t len,
                              std::vector<protocol::tile_blob>& tiles,
                              std::string& error) {
  (void)payload;
  (void)len;
  tiles.clear();
  error = "this build has no dav1d";
  return false;
}

#endif  // MOLINK_HAVE_DAV1D

}  // namespace molink
