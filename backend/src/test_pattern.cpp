#include "test_pattern.h"

#include <algorithm>
#include <utility>

#include "protocol/frame_packet.h"

namespace molink {
namespace {

inline uint8_t clamp_byte(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

bool build_test_frame_packet(uint32_t session, uint32_t seq, uint64_t ts_us,
                             const test_pattern_options& opts,
                             std::vector<uint8_t>& out, std::string& err) {
  if (opts.width == 0 || opts.height == 0 || opts.tile == 0) {
    err = "test pattern dimensions must be non-zero";
    return false;
  }
  if (static_cast<std::size_t>(opts.width) * opts.height * 4u >
      test_pattern_options::kMaxBytes) {
    err = "test pattern exceeds the frame payload budget";
    return false;
  }

  protocol::frame_header fh;
  fh.frame_seq = seq;
  fh.codec = static_cast<uint8_t>(protocol::codec::bgra);
  fh.flags = (seq % 60u == 0u) ? protocol::kFrameFlagKeyframe : 0;
  fh.screen_w = opts.width;
  fh.screen_h = opts.height;
  fh.tile_w = opts.tile;
  fh.tile_h = opts.tile;

  std::vector<protocol::tile_blob> tiles;
  for (uint16_t ty = 0; ty < opts.height; ty += opts.tile) {
    for (uint16_t tx = 0; tx < opts.width; tx += opts.tile) {
      protocol::tile_blob tile;
      tile.x = tx;
      tile.y = ty;
      tile.w = static_cast<uint16_t>(
          std::min<int>(opts.tile, opts.width - tx));
      tile.h = static_cast<uint16_t>(
          std::min<int>(opts.tile, opts.height - ty));
      tile.flags = protocol::kTileFlagChanged |
                   ((fh.flags & protocol::kFrameFlagKeyframe)
                        ? protocol::kTileFlagKeyframe
                        : 0);
      tile.data.resize(static_cast<std::size_t>(tile.w) * tile.h * 4u);

      uint8_t* p = tile.data.data();
      for (uint16_t y = 0; y < tile.h; ++y) {
        const uint16_t gy = static_cast<uint16_t>(ty + y);
        for (uint16_t x = 0; x < tile.w; ++x) {
          const uint16_t gx = static_cast<uint16_t>(tx + x);
          // Hue ramp across the screen so tile seams are obvious.
          const int r = gx * 255 / opts.width;
          const int g = gy * 255 / opts.height;
          // Checkerboard that scrolls with the frame number, proving the
          // displayed frame actually advances.
          const bool checker =
              (((gx / 32) + (gy / 32) + seq) & 1u) != 0u;
          int b = checker ? 200 : 40;
          int rr = r;
          int gg = g;
          // A white bar sweeps horizontally once per 120 frames.
          const int bar = static_cast<int>((gx + seq * 6u) % opts.width);
          if (bar < 6) {
            rr = gg = 255;
            b = 255;
          }
          *p++ = clamp_byte(b);
          *p++ = clamp_byte(gg);
          *p++ = clamp_byte(rr);
          *p++ = 255;
        }
      }
      tiles.push_back(std::move(tile));
    }
  }

  std::vector<uint8_t> payload;
  if (!protocol::build_frame_payload(fh, tiles, payload, err)) return false;

  protocol::packet pkt;
  pkt.type = static_cast<uint8_t>(protocol::packet_type::frame);
  pkt.flags = (fh.flags & protocol::kFrameFlagKeyframe)
                  ? protocol::kPacketFlagKeyframe
                  : 0;
  pkt.session = session;
  pkt.seq = seq;
  pkt.ts_us = ts_us;
  pkt.payload = std::move(payload);
  return protocol::encode_packet(pkt, out, err);
}

}  // namespace molink
