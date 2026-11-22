#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "video_mode.h"

namespace {

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

bool contains(const std::vector<std::string>& values, const char* wanted) {
  return std::find(values.begin(), values.end(), wanted) != values.end();
}

/// Records the forced-mode calls the supervisor makes on the real capture.
struct fake_capture {
  int stream_always_calls = 0;
  bool last_stream_always = false;
  int last_enter_percent = -1;

  void set_stream_always(bool always) {
    ++stream_always_calls;
    last_stream_always = always;
  }
  void set_stream_enter_percent(int percent) {
    last_enter_percent = percent;
  }
};

void test_settings_values_normalise() {
  using molink::normalize_video_mode;
  expect(normalize_video_mode("auto") == "auto", "auto stays auto");
  expect(normalize_video_mode("mixed") == "auto",
         "the legacy mixed spelling loads as auto");
  expect(normalize_video_mode("garbage") == "auto",
         "an unknown value falls back to auto");
  expect(normalize_video_mode("") == "auto",
         "an empty value falls back to auto");
  expect(normalize_video_mode("stream") == "stream",
         "stream stays a pinned stream");
  expect(normalize_video_mode("tiles") == "tiles",
         "tiles stays a pinned tile path");
}

void test_welcome_advertises_both_paths_in_auto() {
  using molink::advertised_stream_codecs;
  using molink::stream_codec_capabilities;
  stream_codec_capabilities host;
  host.av1 = host.vp9 = host.vp8 = host.h264 = true;
  const stream_codec_capabilities viewer = host;

  const std::vector<std::string> auto_codecs =
      advertised_stream_codecs("auto", host, viewer);
  expect(contains(auto_codecs, "tiles"),
         "auto advertises the tile fallback");
  expect(contains(auto_codecs, "av1") || contains(auto_codecs, "vp9") ||
             contains(auto_codecs, "vp8") ||
             contains(auto_codecs, "h264"),
         "auto advertises at least one stream encoder");
  expect(contains(auto_codecs, "av1"),
         "auto advertises every codec both sides support");
  expect(auto_codecs.back() == "tiles",
         "tiles is advertised after the stream codecs");

  const std::vector<std::string> tile_codecs =
      advertised_stream_codecs("tiles", host, viewer);
  expect(tile_codecs.size() == 1 && tile_codecs[0] == "tiles",
         "tiles mode advertises only tiles");

  const std::vector<std::string> stream_codecs =
      advertised_stream_codecs("stream", host, viewer);
  expect(contains(stream_codecs, "av1"),
         "stream mode still advertises the stream encoders");
  expect(contains(stream_codecs, "tiles"),
         "stream mode also advertises the manual tile choice");
  expect(stream_codecs.back() == "tiles",
         "tiles stays last in stream mode");

  stream_codec_capabilities h264_only_viewer;
  h264_only_viewer.h264 = true;
  const std::vector<std::string> mixed_viewer =
      advertised_stream_codecs("auto", host, h264_only_viewer);
  expect(contains(mixed_viewer, "h264") && contains(mixed_viewer, "tiles") &&
             !contains(mixed_viewer, "av1"),
         "auto intersects the host encoders with what the viewer decodes");
}

void test_stream_always_preference_overrides_mode() {
  using molink::stream_always_for;
  expect(stream_always_for("stream", "auto"),
         "stream + auto forces the stream");
  expect(!stream_always_for("stream", "tiles"),
         "an explicit tiles preference wins over a stream default");
  expect(!stream_always_for("auto", "auto"),
         "auto leaves the adaptive policy in charge");
  expect(!stream_always_for("auto", "tiles"),
         "auto + tiles never forces the stream");
  expect(!stream_always_for("tiles", "auto"),
         "tiles never forces the stream");
  expect(!stream_always_for("tiles", "tiles"),
         "tiles + tiles never forces the stream");
  expect(!stream_always_for("mixed", "av1"),
         "the legacy mixed spelling normalises to auto, not stream");
}

void test_auto_releases_stream_always() {
  using molink::apply_video_mode;
  fake_capture capture;
  apply_video_mode(capture, "auto", "auto", 20);
  expect(capture.stream_always_calls == 1,
         "the supervisor applies the video mode to the capture");
  expect(!capture.last_stream_always,
         "auto leaves stream_always false for the adaptive policy");
  expect(capture.last_enter_percent == 20,
         "the movement threshold is still applied");

  apply_video_mode(capture, "stream", "auto", 20);
  expect(capture.last_stream_always,
         "stream pins stream_always true");

  apply_video_mode(capture, "stream", "tiles", 20);
  expect(!capture.last_stream_always,
         "picking tiles in the toolbar releases the forced stream");

  apply_video_mode(capture, "stream", "tiles", 20, true);
  expect(capture.last_stream_always,
         "the secure desktop still forces the stream over a tiles preference");

  apply_video_mode(capture, "tiles", "auto", 20);
  expect(!capture.last_stream_always,
         "tiles also releases the forced stream");
}

}  // namespace

int main() {
  test_settings_values_normalise();
  test_welcome_advertises_both_paths_in_auto();
  test_stream_always_preference_overrides_mode();
  test_auto_releases_stream_always();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
