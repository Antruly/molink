// The unified encoder face: the ladder the capture walks and the backend list
// it can offer. No encoder library is needed -- this is the policy, not the
// codec.
#include <cstdio>
#include <string>
#include <vector>

#include "capture/av1_encoder.h"
#include "capture/video_encoder.h"

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

void test_ladder_order() {
  using molink::video_codec;
  const auto all = molink::video_ladder(true, true, true);
  expect(all.size() == 3, "a viewer set that decodes everything has three rungs");
  expect(all[0] == video_codec::av1_stream, "AV1 stream comes first");
  expect(all[1] == video_codec::h264_stream, "H.264 stream is the second rung");
  expect(all[2] == video_codec::av1_patches, "patches are the last rung");

  const auto h264_only = molink::video_ladder(false, true, false);
  expect(h264_only.size() == 1 && h264_only[0] == video_codec::h264_stream,
         "a viewer without AV1 gets the H.264 stream");

  const auto av1_only = molink::video_ladder(true, false, true);
  expect(av1_only.size() == 2 && av1_only[0] == video_codec::av1_stream &&
             av1_only[1] == video_codec::av1_patches,
         "a viewer without H.264 never sees it in the ladder");

  expect(molink::video_ladder(false, false, false).empty(),
         "no shared decoder leaves the ladder empty");
}

void test_backend_list_is_consistent() {
  const auto backends = molink::video_encoder_backends();
  for (const auto& backend : backends) {
    expect(!backend.id.empty(), "every backend has an id");
    expect(backend.available, "listed backends are available");
    expect(backend.av1_patches || backend.av1_stream || backend.h264_stream,
           "a backend that is listed covers at least one rung");
    if (backend.hardware) {
      expect(!backend.name.empty(), "hardware backends carry a display name");
    }
  }
  // The list must not claim a rung twice with contradictory hardware flags in
  // a way the caller could not resolve: the caller takes the first backend
  // that offers the rung, so a hardware entry has to sort first.
  bool seen_software = false;
  for (const auto& backend : backends) {
    if (!backend.hardware) seen_software = true;
    else expect(!seen_software, "hardware backends are listed before software");
  }
}

void test_codec_names() {
  using molink::video_codec;
  expect(std::string(molink::video_codec_name(video_codec::av1_patches)) ==
             "av1-patches",
         "av1 patches name");
  expect(std::string(molink::video_codec_name(video_codec::av1_stream)) ==
             "av1-stream",
         "av1 stream name");
  expect(std::string(molink::video_codec_name(video_codec::h264_stream)) ==
             "h264-stream",
         "h264 stream name");
}

/// The base class answers "no" for halves a backend does not implement, so the
/// capture can call either method on any encoder.
void test_base_defaults() {
  struct patches_only final : molink::video_encoder {
    molink::video_codec codec() const override {
      return molink::video_codec::av1_patches;
    }
  };
  patches_only encoder;
  std::vector<uint8_t> out;
  bool keyframe = false;
  std::string error;
  const uint8_t pixel[4] = {0, 0, 0, 255};
  expect(!encoder.encode_stream(pixel, 4, 1, 1, 30, true, out, keyframe, error),
         "a patches-only backend refuses the stream call");
  expect(!error.empty(), "and says why");
  error.clear();
  expect(!encoder.encode_intra(pixel, 4, 1, 1, 30, out, error),
         "the base class refuses an intra call the backend does not implement");
  expect(!error.empty(), "and says why");
  expect(!encoder.hardware(), "backends are software unless they say otherwise");
}

}  // namespace

int main() {
  test_ladder_order();
  test_backend_list_is_consistent();
  test_codec_names();
  test_base_defaults();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
