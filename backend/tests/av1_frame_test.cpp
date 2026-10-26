// AV1 FRAME payload round-trip: encode screen rectangles into one AV1 frame,
// parse it, decode it back to BGRA tiles, and compare the geometry and the
// average colour. Built only when the AV1 SDK is available.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "capture/av1_encoder.h"
#include "capture/av1_frame.h"
#include "protocol/frame_packet.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void expect(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  }
}

struct colour {
  double b = 0;
  double g = 0;
  double r = 0;
};

colour mean_colour(const uint8_t* bgra, int width, int height, int stride) {
  double b = 0;
  double g = 0;
  double r = 0;
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = bgra + static_cast<std::size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      const uint8_t* px = row + static_cast<std::size_t>(x) * 4;
      b += px[0];
      g += px[1];
      r += px[2];
    }
  }
  const double count = static_cast<double>(width) * height;
  return {b / count, g / count, r / count};
}

void test_roundtrip() {
  constexpr int kSize = 256;
  constexpr int kRect = 128;
  std::vector<uint8_t> screen(static_cast<std::size_t>(kSize) * kSize * 4);
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      uint8_t* px = screen.data() +
                    (static_cast<std::size_t>(y) * kSize + x) * 4;
      if (x < kRect && y < kRect) {
        px[0] = 20;
        px[1] = 40;
        px[2] = 200;
      } else if (x >= kRect && y >= kRect) {
        px[0] = 200;
        px[1] = 60;
        px[2] = 30;
      } else {
        px[0] = 90;
        px[1] = 140;
        px[2] = 90;
      }
      px[3] = 255;
    }
  }

  std::string error;
  auto encoder = molink::make_av1_encoder(&error);
  expect(encoder != nullptr, "AV1 encoder available: " + error);
  if (encoder == nullptr) return;

  molink::protocol::frame_header header;
  header.frame_seq = 7;
  header.screen_w = kSize;
  header.screen_h = kSize;
  header.tile_w = kRect;
  header.tile_h = kRect;
  const std::vector<molink::pixel_rect> rects = {
      {0, 0, kRect, kRect}, {kRect, kRect, kRect, kRect}};

  std::vector<uint8_t> payload;
  expect(molink::build_av1_frame_payload(header, *encoder, screen.data(),
                                         kSize * 4, rects, 30, payload,
                                         error),
         "AV1 frame payload built: " + error);
  if (payload.empty()) return;

  molink::protocol::parsed_frame parsed;
  expect(molink::protocol::parse_frame_payload(payload.data(), payload.size(),
                                               parsed, error),
         "AV1 frame payload parses: " + error);
  expect(parsed.header.codec ==
             static_cast<uint8_t>(molink::protocol::codec::av1),
         "payload codec is av1");
  expect(parsed.tiles.size() == 2, "payload has two AV1 tiles");
  if (parsed.tiles.size() != 2) return;
  expect(parsed.tiles[0].x == 0 && parsed.tiles[0].y == 0 &&
             parsed.tiles[0].w == kRect && parsed.tiles[0].h == kRect,
         "first rectangle geometry survives");
  expect(parsed.tiles[1].x == kRect && parsed.tiles[1].y == kRect,
         "second rectangle geometry survives");

  std::vector<molink::protocol::tile_blob> decoded;
  expect(molink::decode_av1_frame_payload(payload.data(), payload.size(),
                                          decoded, error),
         "AV1 frame payload decodes: " + error);
  expect(decoded.size() == 2, "decoded two tiles");
  if (decoded.size() != 2) return;
  for (std::size_t i = 0; i < decoded.size(); ++i) {
    const auto& tile = decoded[i];
    expect(tile.x == rects[i].x && tile.y == rects[i].y &&
               tile.w == rects[i].w && tile.h == rects[i].h,
           "decoded tile " + std::to_string(i) + " keeps its geometry");
    expect(tile.data.size() ==
               static_cast<std::size_t>(tile.w) * tile.h * 4,
           "decoded tile " + std::to_string(i) + " is BGRA");
    bool opaque = true;
    for (std::size_t p = 3; p < tile.data.size(); p += 4) {
      if (tile.data[p] != 255) {
        opaque = false;
        break;
      }
    }
    expect(opaque, "decoded tile " + std::to_string(i) + " is opaque");
  }

  const colour source0 =
      mean_colour(screen.data(), kRect, kRect, kSize * 4);
  const colour decoded0 =
      mean_colour(decoded[0].data.data(), kRect, kRect, kRect * 4);
  expect(std::abs(source0.b - decoded0.b) < 24 &&
             std::abs(source0.g - decoded0.g) < 24 &&
             std::abs(source0.r - decoded0.r) < 24,
         "decoded colour is close to the source");

  const uint8_t* second_source =
      screen.data() + (static_cast<std::size_t>(kRect) * kSize + kRect) * 4;
  const colour source1 = mean_colour(second_source, kRect, kRect, kSize * 4);
  const colour decoded1 =
      mean_colour(decoded[1].data.data(), kRect, kRect, kRect * 4);
  expect(std::abs(source1.b - decoded1.b) < 24 &&
             std::abs(source1.g - decoded1.g) < 24 &&
             std::abs(source1.r - decoded1.r) < 24,
         "second decoded colour is close to the source");
}

void test_rejects_non_av1() {
  molink::protocol::frame_header header;
  header.frame_seq = 1;
  header.codec = static_cast<uint8_t>(molink::protocol::codec::bgra);
  std::vector<molink::protocol::tile_blob> tiles(1);
  tiles[0].w = 4;
  tiles[0].h = 4;
  tiles[0].data.assign(64, 0);
  std::vector<uint8_t> payload;
  std::string error;
  expect(molink::protocol::build_frame_payload(header, tiles, payload, error),
         "BGRA frame payload builds");
  std::vector<molink::protocol::tile_blob> decoded;
  expect(!molink::decode_av1_frame_payload(payload.data(), payload.size(),
                                           decoded, error),
         "a BGRA payload is rejected by the AV1 decoder");
}

}  // namespace

int main() {
  test_roundtrip();
  test_rejects_non_av1();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
