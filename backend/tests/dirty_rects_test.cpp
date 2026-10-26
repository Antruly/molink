// Dirty-tile rectangle merging tests. No framework: this is the executable
// specification for the merger used by the AV1 rectangle path.
#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "capture/dirty_rects.h"

namespace {

using molink::merge_dirty_tiles;
using molink::pixel_rect;

int g_failures = 0;
int g_checks = 0;

void expect(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  }
}

bool contains(const pixel_rect& outer, int x, int y) {
  return x >= outer.x && x < outer.x + outer.w && y >= outer.y &&
         y < outer.y + outer.h;
}

bool covers_cell(const std::vector<pixel_rect>& rects, int col, int row,
                 int tile_w, int tile_h) {
  const int x = col * tile_w;
  const int y = row * tile_h;
  return std::any_of(rects.begin(), rects.end(),
                     [x, y](const pixel_rect& rect) {
                       return contains(rect, x, y);
                     });
}

bool intersects(const pixel_rect& a, const pixel_rect& b) {
  const int x0 = std::max(a.x, b.x);
  const int y0 = std::max(a.y, b.y);
  const int x1 = std::min(a.x + a.w, b.x + b.w);
  const int y1 = std::min(a.y + a.h, b.y + b.h);
  return x1 > x0 && y1 > y0;
}

bool disjoint(const std::vector<pixel_rect>& rects) {
  for (std::size_t i = 0; i < rects.size(); ++i) {
    for (std::size_t j = i + 1; j < rects.size(); ++j) {
      if (intersects(rects[i], rects[j])) return false;
    }
  }
  return true;
}

void test_empty() {
  expect(merge_dirty_tiles({}, 128, 128, 1920, 1080).empty(),
         "no dirty cells yields no rectangles");
}

void test_single_cell() {
  const auto rects = merge_dirty_tiles({{2, 1}}, 128, 128, 1920, 1080);
  expect(rects.size() == 1, "one dirty cell yields one rectangle");
  expect(rects[0].x == 256 && rects[0].y == 128 && rects[0].w == 128 &&
             rects[0].h == 128,
         "single cell maps to its pixel rectangle");
}

void test_row_run() {
  const auto rects =
      merge_dirty_tiles({{0, 0}, {1, 0}, {2, 0}}, 128, 128, 1920, 1080);
  expect(rects.size() == 1, "a horizontal run merges into one rectangle");
  expect(rects[0].w == 384 && rects[0].h == 128,
         "horizontal run keeps tile height");
}

void test_block() {
  const auto rects = merge_dirty_tiles(
      {{0, 0}, {1, 0}, {0, 1}, {1, 1}}, 128, 128, 1920, 1080);
  expect(rects.size() == 1, "a 2x2 block merges into one rectangle");
  expect(rects[0].w == 256 && rects[0].h == 256, "block covers four tiles");
}

void test_l_shape() {
  const auto rects =
      merge_dirty_tiles({{0, 0}, {1, 0}, {0, 1}}, 128, 128, 1920, 1080);
  expect(rects.size() == 2, "an L shape yields two rectangles");
  expect(disjoint(rects), "L shape rectangles do not overlap");
  expect(covers_cell(rects, 0, 0, 128, 128), "L shape covers its first cell");
  expect(covers_cell(rects, 1, 0, 128, 128), "L shape covers its second cell");
  expect(covers_cell(rects, 0, 1, 128, 128), "L shape covers its third cell");
}

void test_duplicates() {
  const auto rects =
      merge_dirty_tiles({{0, 0}, {0, 0}, {0, 0}}, 128, 128, 1920, 1080);
  expect(rects.size() == 1, "duplicate cells still yield one rectangle");
  expect(rects[0].w == 128 && rects[0].h == 128,
         "duplicate cells do not grow the rectangle");
}

void test_out_of_range() {
  const auto rects = merge_dirty_tiles({{-1, 0}, {0, -1}}, 128, 128, 1920, 1080);
  expect(rects.empty(), "negative cells are ignored");
}

void test_clipping() {
  // 200x200 screen with 128 tiles: cell (1, 0) only has 72 pixels of width.
  const auto rects = merge_dirty_tiles({{1, 0}}, 128, 128, 200, 200);
  expect(rects.size() == 1, "edge cell yields one rectangle");
  expect(rects[0].x == 128 && rects[0].w == 72, "rectangle is clipped to the screen");
  const auto bottom = merge_dirty_tiles({{0, 1}}, 128, 128, 200, 200);
  expect(bottom.size() == 1 && bottom[0].y == 128 && bottom[0].h == 72,
         "bottom edge cell is clipped vertically");
}

void test_count_limit_and_coverage() {
  std::vector<std::pair<int, int>> cells;
  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      if ((row * 8 + col) % 3 == 0) cells.emplace_back(col, row);
    }
  }
  const int tile = 64;
  const auto rects = merge_dirty_tiles(cells, tile, tile, 512, 512, 4);
  expect(rects.size() <= 4, "coarsening keeps the rectangle count bounded");
  expect(disjoint(rects), "coarsened rectangles do not overlap");
  bool all_covered = true;
  for (const std::pair<int, int>& cell : cells) {
    if (!covers_cell(rects, cell.first, cell.second, tile, tile)) {
      all_covered = false;
    }
  }
  expect(all_covered, "coarsened rectangles still cover every dirty cell");
}

void test_single_rect_limit() {
  const auto rects =
      merge_dirty_tiles({{0, 0}, {5, 0}, {0, 5}, {5, 5}}, 64, 64, 512, 512, 1);
  expect(rects.size() == 1, "max_rects=1 coarsens to a single rectangle");
  expect(rects[0].x == 0 && rects[0].y == 0 && rects[0].w == 512 &&
             rects[0].h == 512,
         "single rectangle covers the whole grid");
}

}  // namespace

int main() {
  test_empty();
  test_single_cell();
  test_row_run();
  test_block();
  test_l_shape();
  test_duplicates();
  test_out_of_range();
  test_clipping();
  test_count_limit_and_coverage();
  test_single_rect_limit();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
