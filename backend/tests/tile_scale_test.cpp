#include <cstdio>
#include <vector>

#include "capture/tile_scale.h"

namespace {
int failures = 0;
int checks = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void test_averages_each_block_in_place() {
  // 4x2 image: two 2x2 blocks, left black/white chequer, right constant.
  constexpr int w = 4, h = 2;
  std::vector<uint8_t> px(static_cast<std::size_t>(w) * h * 4, 0);
  auto put = [&px, w](int x, int y, uint8_t b, uint8_t g, uint8_t r) {
    uint8_t* p = px.data() + (static_cast<std::size_t>(y) * w + x) * 4;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 255;
  };
  put(0, 0, 0, 0, 0);
  put(1, 0, 200, 200, 200);
  put(0, 1, 100, 100, 100);
  put(1, 1, 100, 100, 100);
  put(2, 0, 40, 40, 40);
  put(3, 0, 40, 40, 40);
  put(2, 1, 40, 40, 40);
  put(3, 1, 40, 40, 40);

  molink::downsample_box2_bgra(px, w, h);

  auto at = [&px, w](int x, int y, int c) {
    return px[(static_cast<std::size_t>(y) * w + x) * 4 + c];
  };
  // (0+200+100+100)/4 = 100, and all four pixels now carry it.
  expect(at(0, 0, 0) == 100 && at(1, 0, 0) == 100 && at(0, 1, 0) == 100 &&
             at(1, 1, 0) == 100,
         "the block becomes its average in place");
  expect(at(0, 0, 3) == 255, "alpha is averaged too (opaque stays opaque)");
  expect(at(2, 0, 0) == 40 && at(3, 1, 2) == 40,
         "a constant block is untouched");
  expect(px.size() == static_cast<std::size_t>(w) * h * 4,
         "the image keeps its size");
}

void test_odd_sizes_and_edges() {
  // 5x3: the last column and row are left alone, nothing may read out of range.
  constexpr int w = 5, h = 3;
  std::vector<uint8_t> px(static_cast<std::size_t>(w) * h * 4, 77);
  molink::downsample_box2_bgra(px, w, h);
  bool untouched = true;
  for (uint8_t value : px) {
    if (value != 77) untouched = false;
  }
  expect(untouched, "a uniform image survives an odd size");

  std::vector<uint8_t> tiny;
  molink::downsample_box2_bgra(tiny, 1, 1);
  expect(tiny.empty(), "a 1x1 image is ignored");
}

void test_short_buffer_is_ignored() {
  std::vector<uint8_t> px(16, 9);  // claims 4x4 but holds 4 pixels
  molink::downsample_box2_bgra(px, 4, 4);
  expect(px.size() == 16 && px[0] == 9, "a short buffer is left alone");
}
}  // namespace

int main() {
  test_averages_each_block_in_place();
  test_odd_sizes_and_edges();
  test_short_buffer_is_ignored();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
