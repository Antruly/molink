// Native picture path: tile frames compose into one BGRA canvas and reach the
// shared-memory surface the shell draws.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "protocol/frame_packet.h"
#include "surface/frame_surface.h"
#include "surface/surface_compositor.h"

namespace {

int checks = 0;
int failures = 0;

void expect(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

molink::protocol::parsed_frame make_frame(int w, int h, uint8_t codec,
                                          molink::protocol::tile_view tile) {
  molink::protocol::parsed_frame frame;
  frame.header.screen_w = static_cast<uint16_t>(w);
  frame.header.screen_h = static_cast<uint16_t>(h);
  frame.header.codec = codec;
  frame.header.tile_count = 1;
  frame.tiles.push_back(tile);
  return frame;
}

}  // namespace

int main() {
  setenv("MOLINK_SURFACE", "1", 1);
  expect(molink::init_frame_surface_from_env(), "the surface is enabled");
  molink::surface_compositor compositor;

  // One BGRA tile at (16,16) painted red: the canvas and the surface must show
  // that rectangle and nothing else.
  std::vector<uint8_t> red(8 * 8 * 4);
  for (std::size_t i = 0; i < red.size(); i += 4) {
    red[i + 0] = 0;
    red[i + 1] = 0;
    red[i + 2] = 255;
    red[i + 3] = 255;
  }
  molink::protocol::tile_view tile;
  tile.x = 16;
  tile.y = 16;
  tile.w = 8;
  tile.h = 8;
  tile.data = red.data();
  tile.size = red.size();
  const auto frame = make_frame(64, 64, 1 /* bgra */, tile);
  expect(compositor.ingest(frame), "a BGRA tile publishes a picture");
  expect(compositor.published() == 1, "one publication");
  expect(molink::frame_surface::instance().sequence() > 0,
         "the surface sequence advanced");

  // RGB565: pure blue must convert to BGRA with the blue channel high.
  uint16_t blue = 0x001f;  // r=0 g=0 b=31
  std::vector<uint8_t> blue565(4 * 4 * 2);
  for (std::size_t i = 0; i < blue565.size(); i += 2) {
    std::memcpy(&blue565[i], &blue, 2);
  }
  molink::protocol::tile_view t2;
  t2.x = 40;
  t2.y = 40;
  t2.w = 4;
  t2.h = 4;
  t2.data = blue565.data();
  t2.size = blue565.size();
  const auto frame2 = make_frame(64, 64, 7 /* rgb565 */, t2);
  expect(compositor.ingest(frame2), "an RGB565 tile publishes");

  // Stream codecs are ignored for now (they need the core-side decoder).
  molink::protocol::tile_view t3 = tile;
  const auto stream = make_frame(64, 64, 16 /* vp9_stream */, t3);
  expect(!compositor.ingest(stream), "stream codecs are not composited yet");

  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
