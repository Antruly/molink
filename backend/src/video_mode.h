#pragma once

#include <string>
#include <vector>

namespace molink {

/// The three host video paths.
///
/// "stream" is the default: it pins the low-delay stream, but the toolbar can
/// still switch to tiles by hand. "tiles" never streams, and "auto" lets
/// capture's stream_policy follow the content: tiles for a still desktop, the
/// stream once enough of the screen keeps moving. "mixed" was the old name
/// for the adaptive path and loads as "auto"; an unknown value does the same
/// instead of silently pinning tiles.
inline std::string normalize_video_mode(const std::string& value) {
  if (value == "stream") return "stream";
  if (value == "tiles") return "tiles";
  return "auto";
}

/// Whether the host should force the stream rather than feed the adaptive
/// policy. A "stream" video mode does that, unless the viewer explicitly
/// picked "tiles" in the toolbar: that choice is a manual override, not an
/// automatic switch, so it must win over the default mode. Auto and tiles
/// leave stream_always_ false, so capture.cpp feeds every frame to
/// stream_policy_.note() and the hysteresis decides.
inline bool stream_always_for(const std::string& video_mode,
                              const std::string& preference) {
  return normalize_video_mode(video_mode) == "stream" &&
         preference != "tiles";
}

/// Which codecs a host can encode and a viewer can decode.
struct stream_codec_capabilities {
  bool av1 = false;
  bool vp9 = false;
  bool vp8 = false;
  bool h264 = false;
};

/// The `stream_codecs` list the welcome publishes. Every mode advertises
/// "tiles": it is both the still-screen fallback in auto mode and the manual
/// "平铺" choice the toolbar keeps offering even while the host defaults to
/// the stream. Stream modes list their encoders first; tiles-only mode lists
/// just "tiles". Order is the codec preference order, with "tiles" last.
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
  // Always offer tiles: even in the pinned stream mode the toolbar keeps a
  // manual "平铺" choice. In tiles-only mode this is the sole entry.
  codecs.push_back("tiles");
  return codecs;
}

/// Applies the host video path to a capture source. A "stream" mode pins
/// stream_always_ unless the viewer chose tiles by hand; auto and tiles both
/// release the forced mode so the adaptive switch owns the decision.
/// `force_stream` is for the secure desktop, which cannot be captured as
/// tiles and therefore ignores a tiles preference.
template <typename Capture>
inline void apply_video_mode(Capture& capture, const std::string& video_mode,
                             const std::string& preference, int enter_percent,
                             bool force_stream = false) {
  capture.set_stream_always(force_stream ||
                            stream_always_for(video_mode, preference));
  capture.set_stream_enter_percent(enter_percent);
}

}  // namespace molink
