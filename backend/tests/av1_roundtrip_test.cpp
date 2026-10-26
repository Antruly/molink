// AV1 rectangle encoder round-trip test: encode a BGRA rectangle as a
// standalone keyframe, decode it with dav1d, and check the picture came back
// at the padded size with roughly the same luma. Built only when the AV1 SDK
// is available; see MOLINK_ENABLE_AV1 in backend/CMakeLists.txt.
#include <cerrno>

#include <dav1d/dav1d.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "capture/av1_encoder.h"

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

std::vector<uint8_t> make_bgra(int width, int height, int base) {
  std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      uint8_t* px = pixels.data() +
                    (static_cast<std::size_t>(y) * width + x) * 4;
      px[0] = static_cast<uint8_t>((x * 3 + base) & 0xff);           // B
      px[1] = static_cast<uint8_t>((y * 5 + base * 2) & 0xff);       // G
      px[2] = static_cast<uint8_t>((x + y + base) & 0xff);           // R
      px[3] = 0xff;
    }
  }
  return pixels;
}

double source_luma_mean(const std::vector<uint8_t>& bgra, int width, int height) {
  double sum = 0;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const uint8_t* px = bgra.data() + (static_cast<std::size_t>(y) * width + x) * 4;
      sum += 0.2126 * px[2] + 0.7152 * px[1] + 0.0722 * px[0];
    }
  }
  return sum / (static_cast<double>(width) * height);
}

struct decoded {
  int width = 0;
  int height = 0;
  double luma_mean = 0;
  bool ok = false;
};

decoded decode_av1(const std::vector<uint8_t>& obu) {
  decoded result;
  Dav1dSettings settings{};
  dav1d_default_settings(&settings);
  settings.n_threads = 1;
  settings.max_frame_delay = 1;
  Dav1dContext* ctx = nullptr;
  if (dav1d_open(&ctx, &settings) != 0) return result;

  Dav1dData data{};
  uint8_t* dst = dav1d_data_create(&data, obu.size());
  if (dst == nullptr) {
    dav1d_close(&ctx);
    return result;
  }
  std::memcpy(dst, obu.data(), obu.size());

  int rc = dav1d_send_data(ctx, &data);
  dav1d_data_unref(&data);
  if (rc < 0 && rc != DAV1D_ERR(EAGAIN)) {
    dav1d_close(&ctx);
    return result;
  }

  Dav1dPicture picture{};
  rc = dav1d_get_picture(ctx, &picture);
  if (rc != 0) {
    dav1d_close(&ctx);
    return result;
  }
  result.width = picture.p.w;
  result.height = picture.p.h;
  const int stride = picture.stride[0];
  const uint8_t* plane = static_cast<const uint8_t*>(picture.data[0]);
  double sum = 0;
  for (int y = 0; y < result.height; ++y) {
    for (int x = 0; x < result.width; ++x) {
      sum += plane[static_cast<std::size_t>(y) * stride + x];
    }
  }
  result.luma_mean = sum / (static_cast<double>(result.width) * result.height);
  result.ok = true;
  dav1d_picture_unref(&picture);
  dav1d_close(&ctx);
  return result;
}

void test_roundtrip(const std::shared_ptr<molink::av1_encoder>& encoder,
                    int width, int height, int base) {
  const std::vector<uint8_t> bgra = make_bgra(width, height, base);
  std::vector<uint8_t> obu;
  std::string error;
  const bool encoded = encoder->encode_intra(
      bgra.data(), static_cast<std::size_t>(width) * 4, width, height, 30,
      obu, error);
  expect(encoded, "encode " + std::to_string(width) + "x" +
                      std::to_string(height) + ": " + error);
  if (!encoded) return;
  expect(!obu.empty(), "encoder produced a non-empty bitstream");

  const decoded picture = decode_av1(obu);
  expect(picture.ok, "dav1d decoded the rectangle");
  if (!picture.ok) return;
  expect(picture.width == molink::av1_padded_extent(width) &&
             picture.height == molink::av1_padded_extent(height),
         "decoded size matches the padded rectangle");

  // Compare the real (unpadded) top-left region with the source.
  const double source = source_luma_mean(bgra, width, height);
  expect(std::fabs(picture.luma_mean - source) < 16.0,
         "decoded luma is close to the source (source " +
             std::to_string(static_cast<int>(source)) + ", decoded " +
             std::to_string(static_cast<int>(picture.luma_mean)) + ")");
}

}  // namespace

int main() {
  std::string error;
  auto encoder = molink::make_av1_encoder(molink::av1_encoder_config{}, &error);
  expect(encoder != nullptr, "AV1 encoder available: " + error);
  if (encoder == nullptr) {
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }

  test_roundtrip(encoder, 128, 128, 10);
  // Same size again exercises encoder reuse; a different size forces a
  // reconfigure.
  test_roundtrip(encoder, 128, 128, 90);
  test_roundtrip(encoder, 200, 120, 40);
  test_roundtrip(encoder, 72, 72, 130);

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
