// Low-delay AV1 stream round trip: encode a few full-screen frames with the
// P-frame encoder and decode them with the stateful decoder. Built only when
// both halves of the AV1 SDK are available.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "capture/av1_encoder.h"
#include "capture/av1_frame.h"

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

std::vector<uint8_t> make_frame(int width, int height, int shift) {
  std::vector<uint8_t> px(static_cast<std::size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      uint8_t* p = px.data() + (static_cast<std::size_t>(y) * width + x) * 4;
      const int sx = (x + shift) % width;
      const int panel = ((sx / 80) + (y / 60)) % 3;
      p[0] = static_cast<uint8_t>(40 + panel * 60);
      p[1] = static_cast<uint8_t>(70 + panel * 40 + ((sx + y) % 20));
      p[2] = static_cast<uint8_t>(110 + panel * 30 + ((sx * 3 + y * 5) % 16));
      p[3] = 255;
    }
  }
  return px;
}

double mean_luma(const std::vector<uint8_t>& bgra) {
  double sum = 0;
  for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
    sum += 0.0722 * bgra[i] + 0.7152 * bgra[i + 1] + 0.2126 * bgra[i + 2];
  }
  return bgra.empty() ? 0 : sum / (bgra.size() / 4);
}

}  // namespace

int main() {
  constexpr int kW = 320;
  constexpr int kH = 240;
  std::string error;
  auto encoder = molink::make_av1_stream_encoder(molink::av1_encoder_config{},
                                                 &error);
  expect(encoder != nullptr, "stream encoder available: " + error);
  if (encoder == nullptr) {
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }

  molink::av1_stream_decoder decoder;
  int decoded_frames = 0;
  long long keyframes = 0;
  long long bytes = 0;
  bool first_packet = true;
  double first_luma = 0;
  for (int frame = 0; frame < 6; ++frame) {
    const auto pixels = make_frame(kW, kH, frame * 6);
    std::vector<uint8_t> packet;
    bool packet_keyframe = false;
    const bool ok = encoder->encode_stream(
        pixels.data(), static_cast<std::size_t>(kW) * 4, kW, kH, 32,
        /*keyframe=*/frame == 0, packet, packet_keyframe, error);
    expect(ok, "frame " + std::to_string(frame) + " encodes: " + error);
    if (!ok) break;
    if (packet.empty()) continue;  // the pipeline still holds this picture
    bytes += static_cast<long long>(packet.size());
    if (packet_keyframe) ++keyframes;
    std::vector<uint8_t> bgra;
    int w = 0;
    int h = 0;
    const bool decoded = decoder.decode(packet.data(), packet.size(),
                                        packet_keyframe, bgra, w, h, error);
    expect(decoded, "frame " + std::to_string(frame) + " decodes: " + error);
    if (!decoded) continue;
    ++decoded_frames;
    expect(w >= kW && h >= kH, "decoded picture covers the screen");
    expect(bgra.size() == static_cast<std::size_t>(w) * h * 4,
           "decoded picture is BGRA");
    if (first_packet) {
      first_packet = false;
      first_luma = mean_luma(bgra);
      expect(packet_keyframe, "the first packet is a keyframe");
    }
  }
  expect(decoded_frames >= 4, "most frames produced a decodable packet");
  expect(keyframes >= 1, "the stream contains a keyframe");
  expect(bytes > 0, "the stream carries bytes");

  // A forced keyframe mid-stream restarts the encoder (SVT ignores the
  // per-picture request), and the resulting packet must be standalone
  // decodable. The pipeline delays output by a couple of pictures, so the
  // keyframe is sent once and then ordinary pictures flush it out.
  std::vector<uint8_t> packet;
  bool packet_keyframe = false;
  bool sent_keyframe = false;
  for (int attempt = 0; attempt < 5 && packet.empty(); ++attempt) {
    const auto pixels = make_frame(kW, kH, 42 + attempt * 2);
    const bool key = !sent_keyframe;
    sent_keyframe = true;
    expect(encoder->encode_stream(pixels.data(),
                                  static_cast<std::size_t>(kW) * 4, kW, kH, 32,
                                  key, packet, packet_keyframe, error),
           "forced keyframe encodes: " + error);
  }
  if (!packet.empty()) {
    std::vector<uint8_t> bgra;
    int w = 0;
    int h = 0;
    expect(decoder.decode(packet.data(), packet.size(), packet_keyframe, bgra,
                          w, h, error),
           "forced keyframe decodes: " + error);
    expect(mean_luma(bgra) > 0, "the forced keyframe paints something");
  } else {
    expect(false, "a forced keyframe produced a packet");
  }
  (void)first_luma;

  // The quality preset hands every stream codec a bitrate budget; without one
  // (the top presets) the encoder keeps constant quality. CPU SVT-AV1 ignored
  // the budget for a while and produced several MB/s at a slideshow pace, so
  // check that a capped encoder really stays near its number.
  struct run_result {
    long long bytes = 0;
    int frames = 0;
    bool ok = true;
  };
  const auto run_stream = [](int budget_kbps) {
    run_result result;
    molink::av1_encoder_config config;
    config.max_bitrate_kbps = budget_kbps;
    std::string run_error;
    auto stream = molink::make_av1_stream_encoder(config, &run_error);
    if (stream == nullptr) {
      expect(false, "encoder with budget " + std::to_string(budget_kbps) +
                        ": " + run_error);
      result.ok = false;
      return result;
    }
    // 60 frames of 640x480 movement at QP 12: constant quality spends a lot,
    // and a 400 kbit/s budget over two seconds of video is 100 KB.
    for (int frame = 0; frame < 60; ++frame) {
      const auto pixels = make_frame(kW * 2, kH * 2, frame * 37);
      std::vector<uint8_t> packet;
      bool packet_keyframe = false;
      if (!stream->encode_stream(pixels.data(),
                                 static_cast<std::size_t>(kW * 2) * 4, kW * 2,
                                 kH * 2, 12, /*keyframe=*/frame == 0, packet,
                                 packet_keyframe, run_error)) {
        expect(false, "budget " + std::to_string(budget_kbps) +
                          " encode: " + run_error);
        result.ok = false;
        return result;
      }
      if (!packet.empty()) {
        result.bytes += static_cast<long long>(packet.size());
        ++result.frames;
      }
    }
    return result;
  };
  const run_result uncapped = run_stream(0);
  const run_result capped = run_stream(400);
  std::printf("stream bytes: uncapped %lld over %d frames, 400 kbit/s %lld "
              "over %d frames\n",
              uncapped.bytes, uncapped.frames, capped.bytes, capped.frames);
  expect(uncapped.ok && capped.ok, "both budget runs encode");
  expect(capped.frames > 0, "the capped stream produced packets");
  if (uncapped.ok && capped.ok) {
    expect(capped.bytes < uncapped.bytes,
           "the budget shrinks the stream on moving content");
    // Two seconds of 400 kbit/s is 100 KB; allow room for the opening
    // keyframe and the rate controller settling.
    expect(capped.bytes <= 300LL * 1024,
           "the capped stream stays near its budget");
  }

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
