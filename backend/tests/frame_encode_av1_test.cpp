// Verifies the capture-side AV1 split: a large changed area becomes one AV1
// patch and is removed from the base tile payload, while a small change stays
// on the tile path. Built only when the AV1 SDK is available.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "capture/av1_encoder.h"
#include "capture/av1_frame.h"
#include "capture/frame_encode.h"
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

void fill_rect(std::vector<uint8_t>& pixels, int stride, int x, int y, int w,
               int h, uint8_t r, uint8_t g, uint8_t b) {
  for (int row = y; row < y + h; ++row) {
    for (int col = x; col < x + w; ++col) {
      uint8_t* px = pixels.data() + static_cast<std::size_t>(row) * stride +
                    static_cast<std::size_t>(col) * 4;
      px[0] = b;
      px[1] = g;
      px[2] = r;
      px[3] = 255;
    }
  }
}

bool covered(int x, int y, const std::vector<molink::protocol::tile_view>& tiles) {
  for (const auto& tile : tiles) {
    if (x >= tile.x && x < tile.x + tile.w && y >= tile.y &&
        y < tile.y + tile.h) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  constexpr int kSize = 512;
  constexpr int kStride = kSize * 4;
  std::vector<uint8_t> pixels(kSize * kSize * 4);
  fill_rect(pixels, kStride, 0, 0, kSize, kSize, 10, 20, 30);
  std::vector<uint8_t> previous = pixels;
  // A 256x256 (2x2 tiles) change qualifies for AV1; a 32x32 change inside a
  // single tile must stay on the tile path.
  fill_rect(pixels, kStride, 0, 0, 256, 256, 200, 40, 40);
  fill_rect(pixels, kStride, 300, 300, 32, 32, 40, 200, 40);

  molink::protocol::frame_header header;
  header.frame_seq = 1;
  header.flags = molink::protocol::kFrameFlagPartial;
  header.screen_w = kSize;
  header.screen_h = kSize;
  header.tile_w = 128;
  header.tile_h = 128;

  std::vector<molink::protocol::tile_blob> tiles;
  molink::build_changed_tiles(pixels.data(), kStride, kSize, kSize, 128,
                              previous.data(), /*keyframe=*/false, nullptr,
                              /*force_opaque=*/false, 0, tiles);

  std::string error;
  auto encoder = molink::make_av1_encoder(molink::av1_encoder_config{}, &error);
  expect(encoder != nullptr, "AV1 encoder available: " + error);
  if (encoder == nullptr) {
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }

  molink::encode_options options;
  options.compress = false;
  options.format = molink::tile_format::bgra;
  options.av1 = encoder.get();
  options.av1_qp = 30;
  options.av1_max_rects = 4;
  options.av1_min_area = 0;  // two tiles

  std::vector<uint8_t> payload;
  std::vector<uint8_t> av1_payload;
  expect(molink::encode_capture_frame(header, tiles, options, pixels.data(),
                                      kStride, payload, &av1_payload, error),
         "frame encodes: " + error);
  expect(!av1_payload.empty(), "large change produced an AV1 patch");
  if (av1_payload.empty()) {
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
  }

  molink::protocol::parsed_frame patch;
  expect(molink::protocol::parse_frame_payload(av1_payload.data(),
                                               av1_payload.size(), patch,
                                               error),
         "AV1 patch parses: " + error);
  expect(patch.header.codec ==
             static_cast<uint8_t>(molink::protocol::codec::av1),
         "patch codec is av1");
  expect(!patch.tiles.empty(), "patch has rectangles");
  for (const auto& tile : patch.tiles) {
    expect(tile.x + tile.w <= 256 && tile.y + tile.h <= 256,
           "patch rectangles cover the large change");
  }

  // The base payload must no longer contain the large-change cells, but the
  // small change must still be there.
  molink::protocol::parsed_frame base;
  expect(molink::protocol::parse_frame_payload(payload.data(), payload.size(),
                                               base, error),
         "base payload parses: " + error);
  expect(covered(300, 300, base.tiles), "small change remains a base tile");
  // 128,128 is inside the large change and inside an AV1 rectangle.
  bool large_in_patch = false;
  for (const auto& tile : patch.tiles) {
    if (128 >= tile.x && 128 < tile.x + tile.w && 128 >= tile.y &&
        128 < tile.y + tile.h) {
      large_in_patch = true;
    }
  }
  expect(large_in_patch, "large change is covered by the patch");
  expect(!covered(128, 128, base.tiles),
         "large change is removed from the base tiles");

  std::vector<molink::protocol::tile_blob> decoded;
  expect(molink::decode_av1_frame_payload(av1_payload.data(),
                                          av1_payload.size(), decoded, error),
         "AV1 patch decodes: " + error);
  expect(!decoded.empty(), "decoded patch has tiles");

  // ---- Full-frame mode: AV1 is the whole picture, tiles are only a fallback.
  // Both the small and the large change must be covered, and the base payload
  // must carry no tiles (the session sends the AV1 packet alone).
  {
    std::vector<uint8_t> previous = pixels;
    fill_rect(pixels, kStride, 0, 0, 256, 256, 30, 60, 200);
    fill_rect(pixels, kStride, 300, 300, 32, 32, 200, 200, 40);
    molink::protocol::frame_header delta = header;
    delta.flags = molink::protocol::kFrameFlagPartial;
    std::vector<molink::protocol::tile_blob> delta_tiles;
    molink::build_changed_tiles(pixels.data(), kStride, kSize, kSize, 128,
                                previous.data(), /*keyframe=*/false, nullptr,
                                /*force_opaque=*/false, 0, delta_tiles);
    molink::encode_options full = options;
    full.av1_full_frame = true;
    std::vector<uint8_t> full_payload;
    std::vector<uint8_t> full_av1;
    bool av1_only = false;
    expect(molink::encode_capture_frame(delta, delta_tiles, full,
                                        pixels.data(), kStride, full_payload,
                                        &full_av1, error, &av1_only),
           "full-frame delta encodes: " + error);
    expect(av1_only, "full-frame delta is AV1-only");
    expect(delta_tiles.empty(), "full-frame delta sends no base tiles");
    molink::protocol::parsed_frame full_base;
    expect(molink::protocol::parse_frame_payload(full_payload.data(),
                                                 full_payload.size(),
                                                 full_base, error),
           "full-frame base payload parses: " + error);
    expect(full_base.tiles.empty(), "full-frame base payload is empty");
    molink::protocol::parsed_frame full_patch;
    expect(molink::protocol::parse_frame_payload(full_av1.data(),
                                                 full_av1.size(),
                                                 full_patch, error),
           "full-frame AV1 payload parses: " + error);
    expect(covered(128, 128, full_patch.tiles),
           "full-frame AV1 covers the large change");
    expect(covered(310, 310, full_patch.tiles),
           "full-frame AV1 covers the small change too");

    // A keyframe has to bootstrap a viewer that has no picture: one rect over
    // the whole screen and the keyframe flag preserved for the receiver.
    std::vector<uint8_t> key_tiles_pixels = pixels;
    molink::protocol::frame_header key_header = header;
    key_header.flags = molink::protocol::kFrameFlagKeyframe;
    std::vector<molink::protocol::tile_blob> key_tiles;
    molink::build_changed_tiles(pixels.data(), kStride, kSize, kSize, 128,
                                key_tiles_pixels.data(), /*keyframe=*/true,
                                nullptr, /*force_opaque=*/false, 0, key_tiles);
    std::vector<uint8_t> key_payload;
    std::vector<uint8_t> key_av1;
    bool key_av1_only = false;
    expect(molink::encode_capture_frame(key_header, key_tiles, full,
                                        pixels.data(), kStride, key_payload,
                                        &key_av1, error, &key_av1_only),
           "full-frame keyframe encodes: " + error);
    expect(key_av1_only, "full-frame keyframe is AV1-only");
    molink::protocol::parsed_frame key_patch;
    expect(molink::protocol::parse_frame_payload(key_av1.data(),
                                                 key_av1.size(), key_patch,
                                                 error),
           "full-frame keyframe AV1 parses: " + error);
    expect((key_patch.header.flags & molink::protocol::kFrameFlagKeyframe) != 0,
           "full-frame keyframe keeps the keyframe flag");
    expect(!key_patch.tiles.empty() && key_patch.tiles[0].x == 0 &&
               key_patch.tiles[0].y == 0 &&
               key_patch.tiles[0].w == kSize &&
               key_patch.tiles[0].h == kSize,
           "full-frame keyframe covers the whole screen");
  }

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
