#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace molink {

struct pixel_rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

/// Merges dirty tile cells (column, row) into disjoint pixel rectangles that
/// cover every dirty cell. Adjacent cells become row spans, a span is extended
/// down while the next row has the exact same span, and when the result still
/// exceeds `max_rects` the grid is coarsened (2x2 blocks, then 4x4, ...) and
/// rebuilt. Coarsening includes some clean pixels but keeps the rectangle count
/// bounded, which is what the AV1 path needs.
///
/// Cells outside the screen are ignored; `tile_w` and `tile_h` must be positive.
/// The result is clipped to `screen_w` x `screen_h`.
std::vector<pixel_rect> merge_dirty_tiles(
    const std::vector<std::pair<int, int>>& dirty_cells,
    int tile_w, int tile_h, int screen_w, int screen_h,
    std::size_t max_rects = 16);

}  // namespace molink
