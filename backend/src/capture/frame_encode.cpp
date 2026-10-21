#include "capture/frame_encode.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include "compression.h"

namespace molink {
namespace {

bool tile_differs(const uint8_t* current, const uint8_t* previous,
                  std::size_t stride, uint16_t w, uint16_t h,
                  int change_threshold) {
  if (change_threshold <= 0) {
    for (uint16_t row = 0; row < h; ++row) {
      const std::size_t at = static_cast<std::size_t>(row) * stride;
      if (std::memcmp(current + at, previous + at,
                      static_cast<std::size_t>(w) * 4) != 0) {
        return true;
      }
    }
    return false;
  }

  // Ignore per-channel deltas at or below the threshold (anti-aliasing and
  // codec noise), and require enough affected pixels that a one-pixel cursor
  // blink in a large tile does not redraw it. The cursor is sent separately.
  const uint32_t total = static_cast<uint32_t>(w) * h;
  const uint32_t min_pixels =
      std::max<uint32_t>(4, total / 2048);
  uint32_t differing = 0;
  for (uint16_t row = 0; row < h; ++row) {
    const uint8_t* a = current + static_cast<std::size_t>(row) * stride;
    const uint8_t* b = previous + static_cast<std::size_t>(row) * stride;
    for (uint16_t x = 0; x < w; ++x, a += 4, b += 4) {
      const int db = a[0] > b[0] ? a[0] - b[0] : b[0] - a[0];
      const int dg = a[1] > b[1] ? a[1] - b[1] : b[1] - a[1];
      const int dr = a[2] > b[2] ? a[2] - b[2] : b[2] - a[2];
      if (db > change_threshold || dg > change_threshold ||
          dr > change_threshold) {
        if (++differing >= min_pixels) return true;
      }
    }
  }
  return false;
}

}  // namespace

void build_changed_tiles(const uint8_t* pixels, std::size_t stride,
                         uint16_t width, uint16_t height, uint16_t tile_size,
                         uint8_t* previous, bool keyframe,
                         const tile_dirty_rect* dirty, bool force_opaque,
                         int change_threshold,
                         std::vector<protocol::tile_blob>& tiles) {
  tiles.clear();
  if (pixels == nullptr || width == 0 || height == 0) return;
  if (!keyframe && previous == nullptr) return;

  const uint16_t tile = std::max<uint16_t>(
      64, tile_size == 0 ? 256 : tile_size);
  const uint32_t cols = (static_cast<uint32_t>(width) + tile - 1) / tile;
  const uint32_t rows = (static_cast<uint32_t>(height) + tile - 1) / tile;
  std::vector<uint8_t> changed(cols, 0);

  for (uint32_t ty = 0; ty < rows; ++ty) {
    const uint32_t y = ty * tile;
    const uint16_t th =
        static_cast<uint16_t>(std::min<uint32_t>(tile, height - y));
    std::fill(changed.begin(), changed.end(), 0);

    for (uint32_t cx = 0; cx < cols; ++cx) {
      const uint32_t x = cx * tile;
      const uint16_t tw =
          static_cast<uint16_t>(std::min<uint32_t>(tile, width - x));
      if (!keyframe && dirty != nullptr && dirty->valid) {
        const bool overlaps =
            static_cast<int>(x) < dirty->x + dirty->width &&
            dirty->x < static_cast<int>(x) + tw &&
            static_cast<int>(y) < dirty->y + dirty->height &&
            dirty->y < static_cast<int>(y) + th;
        if (!overlaps) continue;
      }
      if (!keyframe) {
        const std::size_t offset =
            static_cast<std::size_t>(y) * stride + x * 4;
        if (!tile_differs(pixels + offset, previous + offset, stride, tw, th,
                          change_threshold)) {
          continue;
        }
      }
      changed[cx] = 1;
    }

    // Coalesce each run of changed tiles in this row into one strip.
    for (uint32_t cx = 0; cx < cols;) {
      if (changed[cx] == 0) {
        ++cx;
        continue;
      }
      uint32_t end = cx + 1;
      while (end < cols && changed[end] != 0) ++end;

      const uint32_t x = cx * tile;
      const uint16_t w = static_cast<uint16_t>(
          std::min<uint32_t>(width - x, (end - cx) * tile));
      protocol::tile_blob blob;
      blob.x = static_cast<uint16_t>(x);
      blob.y = static_cast<uint16_t>(y);
      blob.w = w;
      blob.h = th;
      blob.flags = protocol::kTileFlagChanged |
                   (keyframe ? protocol::kTileFlagKeyframe : 0);
      blob.data.resize(static_cast<std::size_t>(w) * th * 4);
      for (uint16_t row = 0; row < th; ++row) {
        const std::size_t at =
            static_cast<std::size_t>(y + row) * stride + x * 4;
        std::memcpy(blob.data.data() + static_cast<std::size_t>(row) * w * 4,
                    pixels + at, static_cast<std::size_t>(w) * 4);
        if (previous != nullptr) {
          std::memcpy(previous + at, pixels + at,
                      static_cast<std::size_t>(w) * 4);
        }
      }
      if (force_opaque) {
        for (std::size_t i = 3; i < blob.data.size(); i += 4) {
          blob.data[i] = 255;
        }
      }
      tiles.push_back(std::move(blob));
      cx = end;
    }
  }
}

bool encode_capture_frame(protocol::frame_header& header,
                          std::vector<protocol::tile_blob>& tiles, bool compress,
                          std::vector<uint8_t>& payload, std::string& err) {
  header.codec = static_cast<uint8_t>(protocol::codec::bgra);

  if (compress && compression_available() && !tiles.empty()) {
    std::size_t raw_bytes = 0;
    for (const auto& tile : tiles) raw_bytes += tile.data.size();

    // Level 1 keeps a keyframe or a burst from stalling the capture thread;
    // small delta frames have time to use a better ratio. zlib is the only
    // stream the browser can decode natively (DecompressionStream("deflate")),
    // so this is the practical quality/latency knob.
    const bool bulk = (header.flags & protocol::kFrameFlagKeyframe) != 0 ||
                      tiles.size() > 24 || raw_bytes > 256 * 1024;
    const int level = bulk ? 1 : (tiles.size() <= 4 ? 6 : 3);

    std::size_t packed_bytes = 0;
    bool all_ok = true;
    std::vector<std::vector<uint8_t>> packed(tiles.size());

    for (std::size_t i = 0; i < tiles.size(); ++i) {
      if (!deflate_buffer(tiles[i].data.data(), tiles[i].data.size(), level,
                          packed[i], err)) {
        all_ok = false;
        break;
      }
      packed_bytes += packed[i].size();
    }

    // Compression has a CPU cost on both ends; skip it when the screen is
    // already incompressible (photos, already-encoded video).
    if (all_ok && packed_bytes < raw_bytes) {
      for (std::size_t i = 0; i < tiles.size(); ++i) {
        tiles[i].data = std::move(packed[i]);
      }
      header.codec = static_cast<uint8_t>(protocol::codec::deflate_bgra);
    } else {
      err.clear();
    }
  }

  return protocol::build_frame_payload(header, tiles, payload, err);
}

}  // namespace molink
