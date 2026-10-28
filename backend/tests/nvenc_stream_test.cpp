// NVENC low-delay stream smoke test.
//
// Encodes a small moving picture through the real GPU session for each codec
// the machine supports and checks the shape the receiver depends on:
//
//   * the first packet is a keyframe and reports itself as one;
//   * later packets are deltas and are not empty;
//   * a forced keyframe mid-stream comes back as a keyframe again.
//
// It does not decode: the AV1/H.264 decoder is on the viewer's side, and the
// Windows build has no dav1d. Exit 77 (CTest: skipped) when NVENC is missing.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "capture/av1_encoder.h"
#include "capture/nvenc_av1_encoder.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void expect(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

constexpr int kWidth = 320;
constexpr int kHeight = 240;

/// BGRA picture with a moving square, so the encoder has something to code.
std::vector<uint8_t> picture(int frame) {
  std::vector<uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4,
                              0x20);
  const int x0 = (frame * 24) % (kWidth - 64);
  const int y0 = (frame * 12) % (kHeight - 64);
  for (int y = y0; y < y0 + 64; ++y) {
    for (int x = x0; x < x0 + 64; ++x) {
      uint8_t* px = pixels.data() +
                    (static_cast<std::size_t>(y) * kWidth + x) * 4;
      px[0] = 32;             // B
      px[1] = static_cast<uint8_t>(128 + frame * 8);  // G
      px[2] = 240;            // R
      px[3] = 255;
    }
  }
  return pixels;
}

/// Runs one codec. Returns false when the encoder could not be created (the
/// caller turns that into a failure only if the codec was reported available).
bool run_codec(molink::video_codec codec, const char* name,
               molink::av1_encoder_config config) {
  std::string error;
  auto encoder = molink::make_stream_encoder(config, codec, &error);
  if (!encoder) {
    std::printf("%s: no encoder (%s)\n", name, error.c_str());
    return false;
  }
  expect(encoder->codec() == codec, "the encoder reports the codec it was asked for");

  const std::vector<uint8_t> first = picture(0);
  std::vector<uint8_t> out;
  bool keyframe = false;

  // NVENC may hold the first picture for a frame; ask a couple of times.
  bool got_first = false;
  for (int attempt = 0; attempt < 3 && !got_first; ++attempt) {
    const std::vector<uint8_t> pixels = picture(attempt);
    if (!encoder->encode_stream(pixels.data(), kWidth * 4, kWidth, kHeight, 30,
                                /*keyframe=*/true, out, keyframe, error)) {
      std::printf("%s: first encode failed: %s\n", name, error.c_str());
      return false;
    }
    if (!out.empty()) got_first = true;
  }
  expect(got_first, "the first keyframe produces a packet");
  expect(keyframe, "the first packet is reported as a keyframe");

  // Deltas: non-empty, not keyframes.
  int delta_packets = 0;
  for (int i = 1; i <= 3; ++i) {
    const std::vector<uint8_t> pixels = picture(i + 3);
    out.clear();
    keyframe = false;
    if (!encoder->encode_stream(pixels.data(), kWidth * 4, kWidth, kHeight, 30,
                                /*keyframe=*/false, out, keyframe, error)) {
      std::printf("%s: delta encode failed: %s\n", name, error.c_str());
      return false;
    }
    if (out.empty()) continue;
    ++delta_packets;
    expect(!keyframe, "a delta packet is not reported as a keyframe");
  }
  expect(delta_packets >= 2, "deltas produce packets");

  // A forced keyframe must come back as one; this is the resync path.
  out.clear();
  keyframe = false;
  const std::vector<uint8_t> resync = picture(9);
  if (encoder->encode_stream(resync.data(), kWidth * 4, kWidth, kHeight, 30,
                             /*keyframe=*/true, out, keyframe, error)) {
    expect(!out.empty() && keyframe, "a forced keyframe comes back as one");
  } else {
    std::printf("%s: resync encode failed: %s\n", name, error.c_str());
    expect(false, "the resync keyframe encodes");
  }
  return true;
}

}  // namespace

int main() {
  const bool av1 = molink::nvenc_av1_available();
  const bool h264 = molink::nvenc_h264_available();
  if (!av1 && !h264) {
    std::printf("SKIP: no NVENC encoder on this machine\n");
    return 77;
  }
  std::printf("nvenc: av1=%d h264=%d\n", av1 ? 1 : 0, h264 ? 1 : 0);

  molink::av1_encoder_config config;
  config.device = "gpu-nvenc";
  config.speed = "fast";

  if (av1) expect(run_codec(molink::video_codec::av1_stream, "av1", config),
                  "the AV1 stream encoder runs");
  if (h264) expect(run_codec(molink::video_codec::h264_stream, "h264", config),
                   "the H.264 stream encoder runs");

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
