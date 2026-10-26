// Smoke test for the NVENC AV1 backend. It encodes one 256x256 gradient and
// writes the raw AV1 temporal unit; the Linux AV1 decoder test can then check
// that dav1d accepts it. Prints SKIP (exit 77) when no NVIDIA encoder is
// present, so CTest can run this on machines without a GPU.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "capture/nvenc_av1_encoder.h"

int main(int argc, char** argv) {
  if (!molink::nvenc_av1_available()) {
    std::printf("SKIP: no AV1-capable NVIDIA encoder\n");
    return 77;
  }
  const auto devices = molink::nvenc_av1_devices();
  for (const auto& device : devices) {
    std::printf("device: %s (%s)\n", device.id.c_str(),
                device.detail.c_str());
  }
  molink::av1_encoder_config config;
  config.device = "gpu-nvenc";
  config.speed = "auto";
  std::string error;
  auto encoder = molink::make_nvenc_av1_encoder(config, &error);
  if (!encoder) {
    std::printf("FAIL: %s\n", error.c_str());
    return 1;
  }
  constexpr int kWidth = 256;
  constexpr int kHeight = 256;
  std::vector<uint8_t> bgra(static_cast<std::size_t>(kWidth) * kHeight * 4);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      uint8_t* px = bgra.data() + (static_cast<std::size_t>(y) * kWidth + x) * 4;
      px[0] = static_cast<uint8_t>(x);        // B
      px[1] = static_cast<uint8_t>(y);        // G
      px[2] = static_cast<uint8_t>(x ^ y);    // R
      px[3] = 255;
    }
  }
  std::vector<uint8_t> payload;
  if (!encoder->encode_intra(bgra.data(), kWidth * 4, kWidth, kHeight, 24,
                             payload, error)) {
    std::printf("FAIL: %s\n", error.c_str());
    return 1;
  }
  const std::string path = argc > 1 ? argv[1] : "nvenc.obu";
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
  out.close();
  std::printf("encoded %zu bytes -> %s\n", payload.size(), path.c_str());
  return payload.empty() ? 1 : 0;
}
