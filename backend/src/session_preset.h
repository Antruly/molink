#pragma once

#include <string>

#include "capture/capture.h"
#include "capture/frame_encode.h"

namespace molink {

/// One viewer-owned picture preset, as it arrives in `session-preset`.
///
/// The preset name is the viewer's fixed tier (turbo/smooth/balanced/hd/
/// truecolor/auto). The remaining fields are the resolved values the viewer
/// wants; an empty/zero field means "take the plan's default". Filling them
/// on the viewer keeps older cores working: a core that only knows the
/// preset name can still resolve it itself.
struct session_preset_request {
  std::string preset;
  /// 1..60, 0 = the preset plan's rate.
  int fps = 0;
  /// QP 0..63, 0 = follow the quality profile.
  int quality = 0;
  /// Stream bitrate ceiling in kbit/s, 0 = follow the profile.
  int bitrate_kbps = 0;
  /// "auto" | "cq" | "cbr" | empty (plan).
  std::string rate_mode;
  /// "" | "preset" | gray8 | rgb565 | rgb24 | bgra.
  std::string format;
  /// "420" | "444" | "gray" | "565" | "auto" | empty (plan).
  std::string chroma;
  /// "auto" (the only value that changes nothing today) or a pinned codec.
  std::string codec;
  /// Filled by normalize_session_preset(), never sent on the wire: this
  /// request keeps a chroma-exact tile path (truecolor) and the 4:2:0 video
  /// stream must stay off. A 4:4:4 request that no encoder can carry is
  /// resolved to 4:2:0 here instead of silently dropping to tiles.
  bool no_subsampling = false;
};

/// What the host actually applied, echoed to the viewer so the toolbar can
/// show real values instead of guessing from the request.
struct session_preset_actual {
  std::string preset;
  int fps = 0;
  std::string quality;
  std::string format;
  std::string chroma;
  std::string codec;
};

/// What the host's video stream encoders can do. Every backend in this tree
/// encodes 4:2:0, so `stream_444` is false unless a future encoder reports it.
/// A 4:4:4 request is then resolved to 4:2:0 (and kept on the stream) rather
/// than being pushed onto the tile path.
struct session_preset_capabilities {
  bool stream_444 = false;
};

/// Validates the wire values and fills every unset field from the preset
/// plan. Returns false with `error` for an unknown preset or an out-of-range
/// value (the session layer answers `bad_preset`).
bool normalize_session_preset(
    const session_preset_request& in, session_preset_request& out,
    std::string& error,
    const session_preset_capabilities& caps = session_preset_capabilities{});

/// The preset the host would use for `name`; also accepts the legacy quality
/// mode spellings so an older viewer that sends `quality` still works.
bool session_preset_quality_mode(const std::string& name,
                                 quality_mode& out);

/// The chroma a tile format carries. The tile path has no chroma
/// subsampling, which is what the 4:4:4 presets rely on.
const char* session_preset_chroma_for_format(tile_format format);

/// The per-session fields a preset owns. They are reset from the resolved
/// request on every switch; nothing is inherited from the previous tier.
struct session_preset_state {
  std::string encoding_preset;
  bool no_subsampling = false;
};

/// Applies a resolved request to a capture-like object and a session state
/// and reports what the capture now holds. `Capture` needs set_quality,
/// set_target_fps, set_stream_overrides, stream_format_override and the
/// getters used for the reply; `Session` needs `encoding_preset` and
/// `no_subsampling`. Both sides are re-checked after the write: a stale
/// override from the previous preset must never survive a switch.
template <typename Capture, typename Session>
session_preset_actual apply_session_preset(Capture& capture, Session& session,
                                           const session_preset_request& req) {
  quality_mode mode = quality_mode::balanced;
  session_preset_quality_mode(req.preset, mode);
  capture.set_quality(mode);
  capture.set_target_fps(req.fps);
  int format = -1;
  if (!req.format.empty() && req.format != "preset") {
    tile_format parsed;
    if (tile_format_from_name(req.format, parsed)) {
      format = static_cast<int>(parsed);
    }
  }
  int rate_mode = 0;
  if (req.rate_mode == "cq") rate_mode = 1;
  else if (req.rate_mode == "cbr") rate_mode = 2;
  capture.set_stream_overrides(req.quality, req.bitrate_kbps, rate_mode,
                               format);

  // The resolved plan owns these two. Assign them rather than OR-ing them
  // with whatever the previous preset (truecolor) left behind.
  session.encoding_preset = req.preset;
  session.no_subsampling = req.no_subsampling;

  // Cross-check the capture. A "preset" request means "follow the quality
  // profile again", i.e. the override must be -1; anything else still in the
  // slot belongs to another tier and is cleared.
  if (capture.stream_format_override() != format) {
    capture.set_stream_overrides(req.quality, req.bitrate_kbps, rate_mode,
                                 format);
  }
  if (session.no_subsampling != req.no_subsampling) {
    session.no_subsampling = req.no_subsampling;
  }

  session_preset_actual actual;
  actual.preset = req.preset;
  actual.fps = capture.target_fps();
  actual.quality = quality_mode_name(capture.quality());
  actual.format = tile_format_name(capture.quality_format());
  actual.chroma = session_preset_chroma_for_format(capture.quality_format());
  return actual;
}

/// Apply without a session (existing callers and unit tests): the derived
/// no_subsampling flag is discarded.
template <typename Capture>
session_preset_actual apply_session_preset(Capture& capture,
                                           const session_preset_request& req) {
  session_preset_state state;
  return apply_session_preset(capture, state, req);
}

/// Whether applying `req` changes the encoder's configuration. A pure frame
/// rate change does not reinitialise the encoder, so it does not need a
/// keyframe; a preset/format/rate/QP change does, and the session layer forces
/// one so the first frame after the change is a complete picture.
template <typename Capture>
bool session_preset_requires_keyframe(const Capture& capture,
                                      const session_preset_request& req) {
  quality_mode mode = quality_mode::balanced;
  session_preset_quality_mode(req.preset, mode);
  if (capture.quality() != mode) return true;
  int format = -1;
  if (!req.format.empty() && req.format != "preset") {
    tile_format parsed;
    if (tile_format_from_name(req.format, parsed)) {
      format = static_cast<int>(parsed);
    }
  }
  if (capture.stream_format_override() != format) return true;
  if (capture.stream_qp() != req.quality) return true;
  if (capture.stream_bitrate_kbps() != req.bitrate_kbps) return true;
  int rate_mode = 0;
  if (req.rate_mode == "cq") rate_mode = 1;
  else if (req.rate_mode == "cbr") rate_mode = 2;
  return capture.stream_rate_mode_value() != rate_mode;
}

}  // namespace molink
