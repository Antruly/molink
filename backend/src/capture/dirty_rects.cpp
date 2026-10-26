#include "capture/dirty_rects.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace molink {
namespace {

struct open_span {
  int col0 = 0;
  int col1 = 0;
  int row0 = 0;
};

int ceil_div(int value, int divisor) {
  return (value + divisor - 1) / divisor;
}

void emit(std::vector<pixel_rect>& out, const open_span& span, int last_row,
          int block_w, int block_h, int screen_w, int screen_h) {
  pixel_rect rect;
  rect.x = span.col0 * block_w;
  rect.y = span.row0 * block_h;
  rect.w = (span.col1 - span.col0 + 1) * block_w;
  rect.h = (last_row - span.row0 + 1) * block_h;
  rect.w = std::min(rect.w, screen_w - rect.x);
  rect.h = std::min(rect.h, screen_h - rect.y);
  if (rect.w > 0 && rect.h > 0) out.push_back(rect);
}

/// Decomposes a block grid into maximal rectangles. Each row contributes its
/// dirty runs; a run that continues unchanged into the next row extends the
/// open rectangle, otherwise the rectangle is emitted. The rectangles never
/// overlap, so every dirty block is covered exactly once.
std::vector<pixel_rect> build_rects(const std::vector<uint8_t>& grid,
                                    int cols, int rows, int block_w,
                                    int block_h, int screen_w, int screen_h) {
  std::vector<pixel_rect> out;
  std::vector<open_span> open;
  for (int row = 0; row <= rows; ++row) {
    std::vector<std::pair<int, int>> runs;
    if (row < rows) {
      int col = 0;
      while (col < cols) {
        if (grid[static_cast<std::size_t>(row) * cols + col] == 0) {
          ++col;
          continue;
        }
        const int start = col;
        while (col + 1 < cols &&
               grid[static_cast<std::size_t>(row) * cols + col + 1] != 0) {
          ++col;
        }
        runs.emplace_back(start, col);
        ++col;
      }
    }

    std::vector<open_span> next;
    next.reserve(open.size() + runs.size());
    for (const open_span& span : open) {
      const bool continues =
          std::find(runs.begin(), runs.end(), std::make_pair(span.col0,
                                                             span.col1)) !=
          runs.end();
      if (continues) {
        next.push_back(span);
      } else {
        emit(out, span, row - 1, block_w, block_h, screen_w, screen_h);
      }
    }
    for (const std::pair<int, int>& run : runs) {
      const bool already_open =
          std::any_of(next.begin(), next.end(),
                      [&run](const open_span& span) {
                        return span.col0 == run.first && span.col1 == run.second;
                      });
      if (!already_open) next.push_back({run.first, run.second, row});
    }
    open = std::move(next);
  }
  return out;
}

std::vector<pixel_rect> merge_at_scale(
    const std::vector<std::pair<int, int>>& dirty_cells, int scale, int tile_w,
    int tile_h, int screen_w, int screen_h) {
  const int block_w = tile_w * scale;
  const int block_h = tile_h * scale;
  const int cols = std::max(1, ceil_div(screen_w, block_w));
  const int rows = std::max(1, ceil_div(screen_h, block_h));
  std::vector<uint8_t> grid(static_cast<std::size_t>(cols) * rows, 0);
  for (const std::pair<int, int>& cell : dirty_cells) {
    if (cell.first < 0 || cell.second < 0) continue;
    const int block_col = cell.first / scale;
    const int block_row = cell.second / scale;
    if (block_col >= cols || block_row >= rows) continue;
    grid[static_cast<std::size_t>(block_row) * cols + block_col] = 1;
  }
  return build_rects(grid, cols, rows, block_w, block_h, screen_w, screen_h);
}

}  // namespace

std::vector<pixel_rect> merge_dirty_tiles(
    const std::vector<std::pair<int, int>>& dirty_cells, int tile_w, int tile_h,
    int screen_w, int screen_h, std::size_t max_rects) {
  std::vector<pixel_rect> none;
  if (dirty_cells.empty() || tile_w <= 0 || tile_h <= 0 || screen_w <= 0 ||
      screen_h <= 0) {
    return none;
  }
  const std::size_t limit = std::max<std::size_t>(1, max_rects);

  // Duplicates would otherwise make the span logic count a block once per
  // repeated cell; the grid handles that, but deduplicating keeps the loop
  // cheap for callers that hand over every touched tile.
  std::vector<std::pair<int, int>> cells = dirty_cells;
  std::sort(cells.begin(), cells.end());
  cells.erase(std::unique(cells.begin(), cells.end()), cells.end());

  std::vector<pixel_rect> rects;
  for (int scale = 1;; scale *= 2) {
    rects = merge_at_scale(cells, scale, tile_w, tile_h, screen_w, screen_h);
    const bool whole_screen = tile_w * scale >= screen_w &&
                              tile_h * scale >= screen_h;
    if (rects.size() <= limit || whole_screen) break;
  }
  return rects;
}

}  // namespace molink
