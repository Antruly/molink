// The libvpx stream encoder: a keyframe, then deltas that only carry what
// changed. Skips (and passes) when the build has no libvpx.
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "capture/av1_encoder.h"
#include "capture/vpx_stream_encoder.h"

#if MOLINK_HAVE_VPX

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

/// A picture with a moving bright square, so deltas have something to code.
void paint(std::vector<uint8_t>& bgra, int width, int height, int frame) {
  bgra.assign(static_cast<std::size_t>(width) * height * 4, 0);
  const int box = 64;
  const int x0 = (frame * 17) % (width - box);
  const int y0 = height / 3;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      uint8_t* px = bgra.data() +
                    (static_cast<std::size_t>(y) * width + x) * 4;
      const bool inside = x >= x0 && x < x0 + box && y >= y0 && y < y0 + box;
      px[0] = inside ? 40 : 120;
      px[1] = inside ? 200 : 120;
      px[2] = inside ? 60 : 120;
      px[3] = 255;
    }
  }
}

void run_codec(molink::video_codec codec, const char* name) {
  molink::av1_encoder_config config;
  config.device = "auto";
  config.threads = 2;
  std::string error;
  std::shared_ptr<molink::av1_stream_encoder> encoder =
      molink::make_vpx_stream_encoder(config, codec, &error);
  expect(encoder != nullptr, "encoder created");
  if (encoder == nullptr) {
    std::fprintf(stderr, "  (%s: %s)\n", name, error.c_str());
    return;
  }
  expect(encoder->codec() == codec, "encoder reports its codec");

  const int width = 320;
  const int height = 180;
  std::vector<uint8_t> bgra;
  std::vector<uint8_t> out;
  bool keyframe = false;

  paint(bgra, width, height, 0);
  const bool key_ok = encoder->encode_stream(
      bgra.data(), width * 4, width, height, 30, /*keyframe=*/true, out,
      keyframe, error);
  if (!key_ok) std::fprintf(stderr, "  (%s keyframe: %s)\n", name, error.c_str());
  expect(key_ok, "keyframe encoded");
  expect(!out.empty(), "keyframe produced a packet");
  expect(keyframe, "keyframe flag set");
  const std::size_t key_bytes = out.size();

  std::size_t delta_bytes = 0;
  for (int frame = 1; frame <= 3; ++frame) {
    paint(bgra, width, height, frame);
    keyframe = true;  // the encoder must clear it for a delta
    expect(encoder->encode_stream(bgra.data(), width * 4, width, height, 30,
                                  /*keyframe=*/false, out, keyframe, error),
           "delta encoded");
    expect(!out.empty(), "delta produced a packet");
    if (frame == 2) delta_bytes = out.size();
  }
  expect(delta_bytes > 0 && delta_bytes < key_bytes,
         "a delta is much smaller than the keyframe");

  std::string message = std::string(" ") + name + ": key=" +
                        std::to_string(key_bytes) + "B delta=" +
                        std::to_string(delta_bytes) + "B";
  std::printf("ok%s\n", message.c_str());
}

}  // namespace

int main() {
  expect(molink::vpx_stream_available(), "libvpx is compiled in");
  run_codec(molink::video_codec::vp9_stream, "vp9");
  run_codec(molink::video_codec::vp8_stream, "vp8");
  std::printf("vpx stream: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}

#else

int main() {
  std::printf("SKIP: libvpx is not compiled in\n");
  return 0;
}

#endif
