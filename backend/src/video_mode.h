#pragma once

#include <string>
#include <vector>

namespace molink {

/// The three host video paths.
///
/// "stream" pins the low-delay stream, "tiles" never streams, and "auto" (the
/// default) lets capture's stream_policy follow the content: tiles for a still
/// desktop, the stream once enough of the screen keeps moving. "mixed" was the
/// old name for the adaptive path and loads as "auto"; an unknown value does
/// the same instead of silently pinning tiles.
inline std::string normalize_video_mode(const std::string& value) {
  if (value == "stream") return "stream";
  if (value == "tiles") return "tiles";
  return "auto";
}

/// "Always stream" is the only mode that overrides the adaptive policy. Auto
/// leaves stream_always_ false, so capture.cpp feeds every frame to
/// stream_policy_.note() and the hysteresis decides.
inline bool stream_always_for_video_mode(const std::string& video_mode) {
  return normalize_video_mode(video_mode) == "stream";
}

/// Which codecs a host can encode and a viewer can decode.
struct stream_codec_capabilities {
  bool av1 = false;
  bool vp9 = false;
  bool vp8 = false;
  bool h264 = false;
};

/// The `stream_codecs` list the welcome publishes. In auto mode the viewer is
/// told about both the encoders it could receive and "tiles", so it knows it
/// may fall back; tiles-only and stream-only modes advertise just their own
/// path. Order is the codec preference order, with "tiles" last.
inline std::vector<std::string> advertised_stream_codecs(
    const std::string& video_mode, const stream_codec_capabilities& host,
    const stream_codec_capabilities& viewer) {
  const std::string mode = normalize_video_mode(video_mode);
  std::vector<std::string> codecs;
  if (mode != "tiles") {
    if (host.av1 && viewer.av1) codecs.push_back("av1");
    if (host.vp9 && viewer.vp9) codecs.push_back("vp9");
    if (host.vp8 && viewer.vp8) codecs.push_back("vp8");
    if (host.h264 && viewer.h264) codecs.push_back("h264");
  }
  if (mode != "stream") codecs.push_back("tiles");
  return codecs;
}

/// Applies the host video path to a capture source. Only an explicit "stream"
/// sets stream_always; auto and tiles both release the forced mode so the
/// adaptive switch owns the decision.
template <typename Capture>
inline void apply_video_mode(Capture& capture, const std::string& video_mode,
                             int enter_percent) {
  capture.set_stream_always(stream_always_for_video_mode(video_mode));
  capture.set_stream_enter_percent(enter_percent);
}

}  // namespace molink
