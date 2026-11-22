#include <cstdio>
#include <string>

#include "session_preset.h"

namespace {

using namespace molink;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

/// Minimal capture face: records what the preset application changed and
/// exposes the same getters the real capture uses for the keyframe decision.
struct fake_capture {
  quality_mode preset = quality_mode::balanced;
  int fps = 15;
  int qp = 0;
  int bitrate = 0;
  int rate_mode = 0;
  int format = -1;
  bool keyframe = false;

  void set_quality(quality_mode mode) { preset = mode; }
  quality_mode quality() const { return preset; }
  void set_target_fps(int value) { fps = value; }
  int target_fps() const { return fps; }
  void set_stream_overrides(int qp_value, int bitrate_value, int rate_value,
                            int format_value) {
    qp = qp_value;
    bitrate = bitrate_value;
    rate_mode = rate_value;
    format = format_value;
  }
  int stream_qp() const { return qp; }
  int stream_bitrate_kbps() const { return bitrate; }
  int stream_rate_mode_value() const { return rate_mode; }
  int stream_format_override() const { return format; }
  tile_format quality_format() const {
    if (format >= 0) return static_cast<tile_format>(format);
    return quality_profile_for(preset).format;
  }
  void request_keyframe() { keyframe = true; }
};

session_preset_request request_for(const char* preset) {
  session_preset_request request;
  request.preset = preset;
  return request;
}

void test_unknown_and_out_of_range_are_rejected() {
  session_preset_request out;
  std::string error;
  expect(!normalize_session_preset(request_for("game"), out, error),
         "an unknown preset is rejected");
  expect(!error.empty(), "the rejection carries a reason");

  for (const int fps : {-1, 61, 999}) {
    session_preset_request request = request_for("hd");
    request.fps = fps;
    expect(!normalize_session_preset(request, out, error),
           "an out-of-range fps is rejected");
  }
  // 0 means "use the plan", not an error.
  session_preset_request automatic = request_for("hd");
  automatic.fps = 0;
  expect(normalize_session_preset(automatic, out, error),
         "fps 0 selects the plan's rate");
  expect(out.fps == 45, "hd defaults to 45 fps");

  session_preset_request bad_qp = request_for("balanced");
  bad_qp.quality = 64;
  expect(!normalize_session_preset(bad_qp, out, error),
         "quality above 63 is rejected");

  session_preset_request bad_rate = request_for("balanced");
  bad_rate.bitrate_kbps = 60001;
  expect(!normalize_session_preset(bad_rate, out, error),
         "bitrate above 60000 is rejected");

  session_preset_request bad_mode = request_for("balanced");
  bad_mode.rate_mode = "vbr";
  expect(!normalize_session_preset(bad_mode, out, error),
         "an unknown rate mode is rejected");

  session_preset_request bad_chroma = request_for("balanced");
  bad_chroma.chroma = "422";
  expect(!normalize_session_preset(bad_chroma, out, error),
         "an unknown chroma is rejected");

  session_preset_request bad_format = request_for("balanced");
  bad_format.format = "rgba";
  expect(!normalize_session_preset(bad_format, out, error),
         "an unknown format is rejected");
}

void test_preset_plans_map_to_host_quality_modes() {
  session_preset_request out;
  std::string error;

  expect(normalize_session_preset(request_for("turbo"), out, error) &&
             out.fps == 10 && out.bitrate_kbps == 2500 &&
             out.chroma == "gray" && out.format.empty(),
         "turbo is low rate, low bitrate and monochrome");

  expect(normalize_session_preset(request_for("smooth"), out, error) &&
             out.fps == 24 && out.chroma == "420",
         "smooth is 24 fps 4:2:0");

  expect(normalize_session_preset(request_for("balanced"), out, error) &&
             out.fps == 30 && out.chroma == "420" && !out.no_subsampling,
         "balanced is 30 fps 4:2:0 on the video stream");

  expect(normalize_session_preset(request_for("hd"), out, error) &&
             out.fps == 45 && out.chroma == "420" && !out.no_subsampling &&
             out.format.empty(),
         "hd is 45 fps high-quality 4:2:0, not a tile-only 4:4:4 request");

  expect(normalize_session_preset(request_for("truecolor"), out, error) &&
             out.fps == 18 && out.chroma == "444" &&
             out.format == "bgra" && out.rate_mode == "cq" &&
             out.no_subsampling,
         "truecolour is 18 fps constant-quality BGRA and keeps the stream off");

  expect(normalize_session_preset(request_for("auto"), out, error) &&
             out.chroma == "auto" && !out.no_subsampling &&
             out.format.empty(),
         "auto resolves to the host's ladder and no chroma restriction");

  quality_mode mode = quality_mode::balanced;
  expect(session_preset_quality_mode("truecolor", mode) &&
             mode == quality_mode::lossless,
         "truecolour maps to the lossless profile");
  expect(session_preset_quality_mode("auto", mode) &&
             mode == quality_mode::automatic,
         "auto maps to the host's automatic ladder");
  expect(!session_preset_quality_mode("nope", mode),
         "unknown preset names do not resolve");
}

void test_explicit_overrides_win() {
  session_preset_request request = request_for("hd");
  request.fps = 12;
  request.chroma = "420";
  request.format = "rgb565";
  session_preset_request out;
  std::string error;
  expect(normalize_session_preset(request, out, error) && out.fps == 12 &&
             out.chroma == "420" && out.format == "rgb565",
         "explicit fps/chroma/format values are kept");

  // A 4:4:4 request with no explicit tile format cannot stream (no encoder
  // carries 4:4:4 here): it resolves to 4:2:0 and stays on the video path,
  // rather than silently dropping to tiles.
  session_preset_request four = request_for("smooth");
  four.chroma = "444";
  four.format.clear();
  expect(normalize_session_preset(four, out, error) && out.chroma == "420" &&
             out.format.empty() && !out.no_subsampling,
         "4:4:4 without an encoder falls back to 4:2:0, not tiles");

  // ...but an encoder that really carries 4:4:4 keeps the request.
  session_preset_capabilities caps;
  caps.stream_444 = true;
  four = request_for("hd");
  four.chroma = "444";
  four.format = "preset";
  expect(normalize_session_preset(four, out, error, caps) &&
             out.chroma == "444" && !out.no_subsampling,
         "4:4:4 stays on the stream when an encoder supports it");

  // A 4:4:4 request with an explicit chroma-preserving tile format is the
  // truecolour path even when the host has no 4:4:4 stream encoder.
  four = request_for("truecolor");
  four.chroma = "444";
  four.format = "bgra";
  expect(normalize_session_preset(four, out, error, caps) && out.chroma == "444" &&
             out.format == "bgra" && out.no_subsampling,
         "truecolour keeps its BGRA tile path regardless of stream capability");
}

void test_apply_rebuilds_and_needs_one_keyframe() {
  fake_capture capture;
  session_preset_request resolved;
  std::string error;
  expect(normalize_session_preset(request_for("truecolor"), resolved, error),
         "truecolour request normalises");

  const bool first_reinit = session_preset_requires_keyframe(capture, resolved);
  expect(first_reinit, "a preset change reinitialises the encoder");
  const session_preset_actual actual =
      apply_session_preset(capture, resolved);
  expect(actual.fps == 18 && actual.quality == "lossless" &&
             actual.format == "bgra" && actual.chroma == "444",
         "the reply reports the actual values");
  // The session layer forces the first frame after a rebuild to be a keyframe.
  if (first_reinit) capture.request_keyframe();
  expect(capture.keyframe, "the first frame after the rebuild is a keyframe");

  // Re-applying the same preset (the viewer's once-a-second fps refresh) does
  // not reinitialise the encoder or force another keyframe.
  capture.keyframe = false;
  expect(!session_preset_requires_keyframe(capture, resolved),
         "an unchanged preset does not reinitialise the encoder");
  session_preset_request same_rate = resolved;
  same_rate.fps = 16;
  expect(!session_preset_requires_keyframe(capture, same_rate),
         "an fps-only change needs no keyframe");
}

void test_truecolor_then_auto_resets_the_stream_gate() {
  fake_capture capture;
  session_preset_state session;
  session_preset_request resolved;
  std::string error;

  // Truecolour: the pixel-exact BGRA tile path, stream suppressed.
  expect(normalize_session_preset(request_for("truecolor"), resolved, error),
         "the truecolour request normalises");
  apply_session_preset(capture, session, resolved);
  expect(session.no_subsampling, "truecolour suppresses the stream");
  expect(session.encoding_preset == "truecolor",
         "truecolour owns the session preset");
  expect(capture.stream_format_override() ==
             static_cast<int>(tile_format::bgra),
         "truecolour installs the BGRA override");

  // Switching back to auto must not inherit truecolour's chroma decision or
  // its format override. This is the "auto stayed on tiles 4:4:4" bug.
  expect(normalize_session_preset(request_for("auto"), resolved, error),
         "the auto request normalises");
  apply_session_preset(capture, session, resolved);
  const bool live = true;
  const bool can_stream = true;
  const bool stream_allowed = live && can_stream && !session.no_subsampling;
  expect(!session.no_subsampling, "auto clears no_subsampling");
  expect(stream_allowed, "the video stream is allowed again after auto");
  expect(capture.stream_format_override() == -1,
         "auto drops truecolour's BGRA override");
  expect(session.encoding_preset == "auto", "auto owns the session preset");
}

}  // namespace

int main() {
  test_unknown_and_out_of_range_are_rejected();
  test_preset_plans_map_to_host_quality_modes();
  test_explicit_overrides_win();
  test_apply_rebuilds_and_needs_one_keyframe();
  test_truecolor_then_auto_resets_the_stream_gate();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
