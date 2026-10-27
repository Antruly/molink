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

  // A forced keyframe mid-stream resets the decoder and still decodes.
  const auto pixels = make_frame(kW, kH, 42);
  std::vector<uint8_t> packet;
  bool packet_keyframe = false;
  for (int attempt = 0; attempt < 3 && packet.empty(); ++attempt) {
    expect(encoder->encode_stream(pixels.data(),
                                  static_cast<std::size_t>(kW) * 4, kW, kH, 32,
                                  /*keyframe=*/true, packet, packet_keyframe,
                                  error),
           "forced keyframe encodes: " + error);
    if (packet.empty()) {
      // Feed another picture to flush the pipeline.
      const auto filler = make_frame(kW, kH, 48 + attempt);
      expect(encoder->encode_stream(filler.data(),
                                    static_cast<std::size_t>(kW) * 4, kW, kH,
                                    32, /*keyframe=*/true, packet,
                                    packet_keyframe, error),
             "flush encodes: " + error);
    }
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

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
