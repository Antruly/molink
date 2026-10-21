#include "capture/frame_encode.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "compression.h"

namespace molink {
namespace {

/// Repack one BGRA tile into a smaller wire layout. Alpha is dropped in both
/// lossy formats; rgb565 also keeps only 5/6/5 bits per channel. The browser
/// expands these back to RGBA while painting.
void pack_tile_pixels(const std::vector<uint8_t>& bgra, tile_format format,
                      std::vector<uint8_t>& out) {
  const std::size_t pixels = bgra.size() / 4;
  if (format == tile_format::rgb565) {
    out.resize(pixels * 2);
    std::size_t at = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
      const std::size_t src = i * 4;
      const uint8_t b = bgra[src + 0];
      const uint8_t g = bgra[src + 1];
      const uint8_t r = bgra[src + 2];
      const uint16_t value =
          static_cast<uint16_t>((static_cast<uint16_t>(r >> 3) << 11) |
                                (static_cast<uint16_t>(g >> 2) << 5) |
                                static_cast<uint16_t>(b >> 3));
      out[at++] = static_cast<uint8_t>(value & 0xff);
      out[at++] = static_cast<uint8_t>(value >> 8);
    }
    return;
  }

  out.resize(pixels * 3);
  std::size_t at = 0;
  for (std::size_t i = 0; i < pixels; ++i) {
    const std::size_t src = i * 4;
    out[at++] = bgra[src + 2];  // R
    out[at++] = bgra[src + 1];  // G
    out[at++] = bgra[src + 0];  // B
  }
}

uint8_t codec_for(tile_format format, bool deflated) {
  using protocol::codec;
  switch (format) {
    case tile_format::rgb24:
      return static_cast<uint8_t>(deflated ? codec::deflate_rgb24
                                           : codec::rgb24);
    case tile_format::rgb565:
      return static_cast<uint8_t>(deflated ? codec::deflate_rgb565
                                           : codec::rgb565);
    case tile_format::bgra:
    default:
      return static_cast<uint8_t>(deflated ? codec::deflate_bgra : codec::bgra);
  }
}

int choose_level(compression_effort effort, bool bulk, std::size_t tiles) {
  switch (effort) {
    case compression_effort::fast:
      // A single level keeps keyframes and bursts from stalling capture.
      return 1;
    case compression_effort::best:
      return bulk ? 3 : (tiles <= 4 ? 9 : 6);
    case compression_effort::balanced:
    default:
      return bulk ? 1 : (tiles <= 4 ? 6 : 3);
  }
}

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
                          std::vector<protocol::tile_blob>& tiles,
                          const encode_options& options,
                          std::vector<uint8_t>& payload, std::string& err) {
  // Lossy repacking happens before compression so zlib sees the smaller,
  // lower-entropy pixel stream. It also means an incompressible frame still
  // ships 2 or 3 bytes per pixel instead of 4.
  if (options.format != tile_format::bgra) {
    std::vector<uint8_t> packed;
    for (auto& tile : tiles) {
      pack_tile_pixels(tile.data, options.format, packed);
      tile.data.swap(packed);
    }
  }
  header.codec = codec_for(options.format, /*deflated=*/false);

  if (options.compress && compression_available() && !tiles.empty()) {
    std::size_t raw_bytes = 0;
    for (const auto& tile : tiles) raw_bytes += tile.data.size();

    const bool keyframe =
        (header.flags & protocol::kFrameFlagKeyframe) != 0;
    const bool bulk = keyframe || tiles.size() > 24 || raw_bytes > 256 * 1024;
    const int level = choose_level(options.effort, bulk, tiles.size());

    std::size_t packed_bytes = 0;
    bool all_ok = true;
    std::vector<std::vector<uint8_t>> packed(tiles.size());

    auto compress_range = [&](std::size_t begin, std::size_t end,
                              std::string& range_err,
                              std::size_t& range_bytes) {
      for (std::size_t i = begin; i < end; ++i) {
        if (!deflate_buffer(tiles[i].data.data(), tiles[i].data.size(), level,
                            packed[i], range_err)) {
          return false;
        }
        range_bytes += packed[i].size();
      }
      return true;
    };

    // A keyframe deflates the whole screen and is the only frame that can
    // exceed its pacing budget. Tiles are independent streams, so spread them
    // over a few threads. Delta frames stay single-threaded: they are already
    // well inside the budget and thread startup would cost more than it saves.
    unsigned workers = 1;
    // Escape hatch for hosts where a short keyframe is less important than
    // keeping every core free (or when debugging the capture thread).
    if (keyframe && std::getenv("MOLINK_SERIAL_KEYFRAME") == nullptr) {
      const unsigned hardware = std::thread::hardware_concurrency();
      workers = std::min<unsigned>(
          {hardware == 0 ? 1u : hardware, 8u,
           static_cast<unsigned>(tiles.size())});
    }

    if (workers > 1) {
      std::vector<std::string> errors(workers);
      std::vector<std::size_t> bytes(workers, 0);
      std::vector<std::thread> threads;
      threads.reserve(workers - 1);
      const std::size_t per = (tiles.size() + workers - 1) / workers;
      auto run = [&](unsigned index) {
        const std::size_t begin = index * per;
        const std::size_t end = std::min(tiles.size(), begin + per);
        if (begin < end) {
          compress_range(begin, end, errors[index], bytes[index]);
        }
      };
      for (unsigned i = 1; i < workers; ++i) {
        threads.emplace_back(run, i);
      }
      run(0);
      for (std::thread& thread : threads) thread.join();
      for (unsigned i = 0; i < workers; ++i) {
        packed_bytes += bytes[i];
        if (!errors[i].empty()) {
          all_ok = false;
          if (err.empty()) err = errors[i];
        }
      }
    } else {
      all_ok = compress_range(0, tiles.size(), err, packed_bytes);
    }

    // Compression has a CPU cost on both ends; skip it when the screen is
    // already incompressible (photos, already-encoded video). The packed raw
    // format is kept in that case: it is still smaller than BGRA.
    if (all_ok && packed_bytes < raw_bytes) {
      for (std::size_t i = 0; i < tiles.size(); ++i) {
        tiles[i].data = std::move(packed[i]);
      }
      header.codec = codec_for(options.format, /*deflated=*/true);
    } else {
      err.clear();
    }
  }

  return protocol::build_frame_payload(header, tiles, payload, err);
}

}  // namespace molink
