#include "capture/frame_encode.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "capture/av1_encoder.h"
#include "capture/av1_frame.h"
#include "capture/dirty_rects.h"
#include "compression.h"

namespace molink {
namespace {

/// Repack one BGRA tile into a smaller wire layout. Alpha is dropped in both
/// lossy formats; rgb565 also keeps only 5/6/5 bits per channel. The browser
/// expands these back to RGBA while painting.
void pack_tile_pixels(const std::vector<uint8_t>& bgra, tile_format format,
                      std::vector<uint8_t>& out) {
  const std::size_t pixels = bgra.size() / 4;
  if (format == tile_format::gray8) {
    out.resize(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
      const std::size_t src = i * 4;
      const uint8_t b = bgra[src + 0];
      const uint8_t g = bgra[src + 1];
      const uint8_t r = bgra[src + 2];
      out[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
    }
    return;
  }
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
    case tile_format::gray8:
      return static_cast<uint8_t>(deflated ? codec::deflate_gray8
                                           : codec::gray8);
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

namespace {

/// Grid cells covered by the changed tiles, in tile coordinates.
std::vector<std::pair<int, int>> collect_dirty_cells(
    const std::vector<protocol::tile_blob>& tiles, int tile_w, int tile_h) {
  std::vector<std::pair<int, int>> cells;
  for (const protocol::tile_blob& tile : tiles) {
    if (tile.w == 0 || tile.h == 0) continue;
    const int col0 = tile.x / tile_w;
    const int col1 = (tile.x + tile.w - 1) / tile_w;
    const int row0 = tile.y / tile_h;
    const int row1 = (tile.y + tile.h - 1) / tile_h;
    for (int row = row0; row <= row1; ++row) {
      for (int col = col0; col <= col1; ++col) cells.emplace_back(col, row);
    }
  }
  return cells;
}

/// Keeps coarsened rectangles from turning two distant changes into one
/// mostly-clean rectangle: at least `min_percent` of the covered cells must
/// actually have changed.
bool dense_enough(const pixel_rect& rect,
                  const std::vector<std::pair<int, int>>& cells, int tile_w,
                  int tile_h, int min_percent) {
  const int col0 = rect.x / tile_w;
  const int col1 = (rect.x + rect.w - 1) / tile_w;
  const int row0 = rect.y / tile_h;
  const int row1 = (rect.y + rect.h - 1) / tile_h;
  const int total = (col1 - col0 + 1) * (row1 - row0 + 1);
  if (total <= 0) return false;
  int dirty = 0;
  for (const std::pair<int, int>& cell : cells) {
    if (cell.first >= col0 && cell.first <= col1 && cell.second >= row0 &&
        cell.second <= row1) {
      ++dirty;
    }
  }
  return dirty * 100 >= total * min_percent;
}

/// Removes the cells covered by `rects` from the tile payload. Delta tiles are
/// one cell row tall, so the remaining pieces are horizontal runs of the
/// original tile; each copy keeps the tile's BGRA rows intact.
void subtract_av1_rects(std::vector<protocol::tile_blob>& tiles,
                        const std::vector<pixel_rect>& rects, int tile_w,
                        int tile_h) {
  std::vector<protocol::tile_blob> out;
  out.reserve(tiles.size());
  for (protocol::tile_blob& tile : tiles) {
    const int row0 = tile.y / tile_h;
    const int row1 = (tile.y + tile.h - 1) / tile_h;
    const int col0 = tile.x / tile_w;
    const int col1 = (tile.x + tile.w - 1) / tile_w;
    if (row0 != row1) {
      out.push_back(std::move(tile));
      continue;
    }
    std::vector<char> excluded(static_cast<std::size_t>(col1 - col0 + 1), 0);
    bool any = false;
    for (const pixel_rect& rect : rects) {
      const int rect_row0 = rect.y / tile_h;
      const int rect_row1 = (rect.y + rect.h - 1) / tile_h;
      if (row0 < rect_row0 || row0 > rect_row1) continue;
      const int rect_col0 = rect.x / tile_w;
      const int rect_col1 = (rect.x + rect.w - 1) / tile_w;
      for (int col = std::max(col0, rect_col0);
           col <= std::min(col1, rect_col1); ++col) {
        excluded[static_cast<std::size_t>(col - col0)] = 1;
        any = true;
      }
    }
    if (!any) {
      out.push_back(std::move(tile));
      continue;
    }
    for (int i = 0; i < static_cast<int>(excluded.size());) {
      if (excluded[static_cast<std::size_t>(i)] != 0) {
        ++i;
        continue;
      }
      int end = i;
      while (end + 1 < static_cast<int>(excluded.size()) &&
             excluded[static_cast<std::size_t>(end + 1)] == 0) {
        ++end;
      }
      const int start_x = (col0 + i) * tile_w;
      const int end_x =
          std::min((col0 + end + 1) * tile_w, tile.x + tile.w);
      const int width = end_x - start_x;
      if (width > 0) {
        protocol::tile_blob sub;
        sub.x = static_cast<uint16_t>(start_x);
        sub.y = tile.y;
        sub.w = static_cast<uint16_t>(width);
        sub.h = tile.h;
        sub.flags = tile.flags;
        sub.data.resize(static_cast<std::size_t>(width) * tile.h * 4);
        const std::size_t source_offset =
            static_cast<std::size_t>(start_x - tile.x) * 4;
        for (int row = 0; row < tile.h; ++row) {
          std::memcpy(
              sub.data.data() + static_cast<std::size_t>(row) * width * 4,
              tile.data.data() +
                  static_cast<std::size_t>(row) * tile.w * 4 + source_offset,
              static_cast<std::size_t>(width) * 4);
        }
        out.push_back(std::move(sub));
      }
      i = end + 1;
    }
  }
  tiles = std::move(out);
}

}  // namespace

bool encode_capture_frame(protocol::frame_header& header,
                          std::vector<protocol::tile_blob>& tiles,
                          const encode_options& options, const uint8_t* pixels,
                          std::size_t stride, std::vector<uint8_t>& payload,
                          std::vector<uint8_t>* av1_payload,
                          std::string& err, bool* av1_only) {
  if (av1_payload != nullptr) av1_payload->clear();
  if (av1_only != nullptr) *av1_only = false;
  const bool keyframe = (header.flags & protocol::kFrameFlagKeyframe) != 0;
  const bool can_encode_av1 =
      options.av1 != nullptr && av1_payload != nullptr && pixels != nullptr &&
      stride > 0 && header.tile_w > 0 && header.tile_h > 0 &&
      (keyframe || !tiles.empty());
  // Full-frame AV1: every changed rectangle goes to the encoder and the tile
  // payload is emptied, so the wire carries one picture and not a tile image
  // plus a patch. Tiles are only used again when AV1 cannot run or fails.
  if (can_encode_av1 && options.av1_full_frame) {
    std::vector<pixel_rect> rects;
    if (keyframe) {
      rects.push_back({0, 0, header.screen_w, header.screen_h});
    } else {
      const std::vector<std::pair<int, int>> cells =
          collect_dirty_cells(tiles, header.tile_w, header.tile_h);
      rects = merge_dirty_tiles(
          cells, header.tile_w, header.tile_h, header.screen_w, header.screen_h,
          static_cast<std::size_t>(std::max(1, options.av1_max_rects)));
      // A picture that is changing nearly everywhere (video, scrolling, a
      // dragged window) is cheaper as one full-screen rectangle than as the
      // several strips the merge produces: every extra rectangle pays its own
      // encoder setup on the host and its own decoder instance on the viewer,
      // and small pictures code worse per pixel. Measured with SVT-AV1: four
      // quarter-screen rectangles take ~1.55x the time of one full-screen
      // picture covering the same area, so above ~60 % changed the full
      // screen wins on both ends.
      long long changed = 0;
      for (const pixel_rect& rect : rects) {
        changed += static_cast<long long>(rect.w) * rect.h;
      }
      const long long screen = static_cast<long long>(header.screen_w) *
                               static_cast<long long>(header.screen_h);
      if (rects.size() > 1 && screen > 0 && changed * 100 >= screen * 60) {
        rects.assign(1, pixel_rect{0, 0, header.screen_w, header.screen_h});
      }
    }
    if (!rects.empty()) {
      std::vector<uint8_t> picture;
      std::string av1_error;
      if (build_av1_frame_payload(header, *options.av1, pixels, stride, rects,
                                  options.av1_qp, picture, av1_error,
                                  options.av1_grayscale)) {
        *av1_payload = std::move(picture);
        tiles.clear();
        if (av1_only != nullptr) *av1_only = true;
        if (std::getenv("MOLINK_AV1_LOG") != nullptr) {
          std::fprintf(stderr, "av1 full: %zu rect(s), %zu bytes\n",
                       rects.size(), av1_payload->size());
        }
      } else if (std::getenv("MOLINK_AV1_LOG") != nullptr) {
        std::fprintf(stderr, "av1 full: encode failed (%s), tiles fallback\n",
                     av1_error.c_str());
      }
    }
  } else if (can_encode_av1 && !keyframe) {
    const std::vector<std::pair<int, int>> cells =
        collect_dirty_cells(tiles, header.tile_w, header.tile_h);
    const std::vector<pixel_rect> rects = merge_dirty_tiles(
        cells, header.tile_w, header.tile_h, header.screen_w, header.screen_h,
        static_cast<std::size_t>(std::max(1, options.av1_max_rects)));
    const int min_area =
        options.av1_min_area > 0
            ? options.av1_min_area
            : 2 * header.tile_w * header.tile_h;
    std::vector<pixel_rect> chosen;
    for (const pixel_rect& rect : rects) {
      if (rect.w * rect.h < min_area) continue;
      if (!dense_enough(rect, cells, header.tile_w, header.tile_h, 25)) {
        continue;
      }
      chosen.push_back(rect);
    }
    if (!chosen.empty() && options.av1_max_area_percent < 100) {
      const long long limit = static_cast<long long>(header.screen_w) *
                              header.screen_h * options.av1_max_area_percent /
                              100;
      long long used = 0;
      std::vector<pixel_rect> capped;
      for (const pixel_rect& rect : chosen) {
        const long long area = static_cast<long long>(rect.w) * rect.h;
        if (used + area > limit) continue;
        used += area;
        capped.push_back(rect);
      }
      chosen.swap(capped);
    }
    if (!chosen.empty()) {
      std::vector<uint8_t> patch;
      std::string av1_error;
      if (build_av1_frame_payload(header, *options.av1, pixels, stride, chosen,
                                  options.av1_qp, patch, av1_error,
                                  options.av1_grayscale)) {
        subtract_av1_rects(tiles, chosen, header.tile_w, header.tile_h);
        *av1_payload = std::move(patch);
        if (std::getenv("MOLINK_AV1_LOG") != nullptr) {
          std::fprintf(stderr, "av1: %zu rect(s), %zu bytes, %zu base tiles\n",
                       chosen.size(), av1_payload->size(), tiles.size());
        }
      }
    }
  }

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
