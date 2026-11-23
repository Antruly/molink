#include "session_preset.h"

namespace molink {
namespace {

struct preset_plan {
  const char* name;
  quality_mode mode;
  int fps;
  int quality;
  int bitrate_kbps;
  const char* rate_mode;
  const char* format;  // "" follows the quality profile
  const char* chroma;
};

/// Mirrors `frontend/src/lib/session-presets.ts`. The viewer sends every
/// resolved field, so this table is only the fallback for fields it omits and
/// for older clients; keep the two in step.
constexpr preset_plan kPlans[] = {
    {"turbo", quality_mode::turbo, 10, 0, 2500, "auto", "", "gray"},
    {"smooth", quality_mode::smooth, 24, 0, 6000, "auto", "", "420"},
    // The fixed colour tiers improve the picture through QP, bitrate and
    // frame rate, not by turning chroma subsampling off: a 4:4:4 request
    // used to disable the whole video stream and drop these tiers onto tiles.
    {"balanced", quality_mode::balanced, 30, 0, 0, "auto", "", "420"},
    {"hd", quality_mode::quality, 45, 0, 0, "auto", "", "420"},
    // Truecolour is deliberately the pixel-exact tile path: BGRA, no chroma
    // subsampling, no video stream.
    {"truecolor", quality_mode::lossless, 18, 0, 0, "cq", "bgra", "444"},
    {"auto", quality_mode::automatic, 30, 0, 0, "auto", "", "auto"},
    // `custom` is not a tier of its own: it means "use exactly the fields the
    // viewer sent". Anything it omits takes this row, which mirrors balanced,
    // so a half-filled or older request still produces a usable picture.
    {"custom", quality_mode::balanced, 30, 0, 0, "auto", "", "420"},
};

const preset_plan* find_plan(const std::string& name) {
  for (const preset_plan& plan : kPlans) {
    if (name == plan.name) return &plan;
  }
  return nullptr;
}

bool valid_rate_mode(const std::string& value) {
  return value == "auto" || value == "cq" || value == "cbr";
}

bool valid_chroma(const std::string& value) {
  return value == "420" || value == "444" || value == "gray" ||
         value == "565" || value == "auto";
}

bool valid_format(const std::string& value) {
  if (value.empty() || value == "preset") return true;
  tile_format parsed;
  return tile_format_from_name(value, parsed);
}

}  // namespace

bool session_preset_quality_mode(const std::string& name, quality_mode& out) {
  const preset_plan* plan = find_plan(name);
  if (plan == nullptr) return false;
  out = plan->mode;
  return true;
}

const char* session_preset_chroma_for_format(tile_format format) {
  switch (format) {
    case tile_format::bgra:
    case tile_format::rgb24:
      return "444";
    case tile_format::rgb565:
      return "565";
    case tile_format::gray8:
    default:
      return "gray";
  }
}

bool normalize_session_preset(
    const session_preset_request& in, session_preset_request& out,
    std::string& error,
    const session_preset_capabilities& caps) {
  const preset_plan* plan = find_plan(in.preset);
  if (plan == nullptr) {
    error =
        "preset must be turbo, smooth, balanced, hd, truecolor, auto or custom";
    return false;
  }
  const bool custom = in.preset == "custom";
  // `custom` uses the wider, explicit ranges the advanced panel offers. The
  // fixed tiers keep their historical bounds (0 = "take the plan").
  if (custom) {
    if (in.fps != 0 && (in.fps < 5 || in.fps > 60)) {
      error = "fps must be 5..60 for a custom preset";
      return false;
    }
  } else if (in.fps < 0 || in.fps > 60) {
    error = "fps must be 1..60";
    return false;
  }
  const int fps = in.fps == 0 ? plan->fps : in.fps;
  if (fps < 1) {
    error = "fps must be 1..60";
    return false;
  }
  if (in.quality < 0 || in.quality > 63) {
    error = "quality must be 0..63";
    return false;
  }
  const int bitrate_max = custom ? 50000 : 60000;
  if (in.bitrate_kbps < 0 || in.bitrate_kbps > bitrate_max) {
    error = custom ? "bitrate_kbps must be 0..50000 for a custom preset"
                   : "bitrate_kbps must be 0..60000";
    return false;
  }
  if (!in.rate_mode.empty() && !valid_rate_mode(in.rate_mode)) {
    error = "rate_mode must be auto, cq or cbr";
    return false;
  }
  if (!valid_format(in.format)) {
    error = "format must be preset, gray8, rgb565, rgb24 or bgra";
    return false;
  }
  if (!in.chroma.empty() && !valid_chroma(in.chroma)) {
    error = "chroma must be 420, 444, gray, 565 or auto";
    return false;
  }
  if (!in.codec.empty() && in.codec != "auto" && in.codec != "av1" &&
      in.codec != "h264" && in.codec != "vp9" && in.codec != "vp8" &&
      in.codec != "tiles") {
    error = "codec must be auto, av1, h264, vp9, vp8 or tiles";
    return false;
  }

  out = in;
  out.fps = fps;
  // Fill the omitted fields from the plan. For `custom` the plan row above is
  // balanced, which is exactly the documented fallback.
  if (out.quality == 0) out.quality = plan->quality;
  if (out.bitrate_kbps == 0) out.bitrate_kbps = plan->bitrate_kbps;
  if (out.rate_mode.empty()) out.rate_mode = plan->rate_mode;
  if (out.format.empty()) out.format = plan->format;
  if (out.chroma.empty()) out.chroma = plan->chroma;
  if (out.codec.empty()) out.codec = "auto";
  // Every request starts from a clean slate. `no_subsampling` is derived from
  // this request alone; it is never carried over from the previous preset
  // (switching truecolor -> auto used to stay on the tile path because the
  // flag was not reset).
  out.no_subsampling = false;
  if (out.chroma == "444") {
    // An explicit BGRA/RGB24 format is the pixel-exact tile path: truecolour
    // asks for it by name and the 4:2:0 stream must stay off.
    const bool explicit_tile_format =
        out.format == "bgra" || out.format == "rgb24";
    if (explicit_tile_format) {
      out.no_subsampling = true;
    } else if (out.codec == "tiles") {
      // The tile path carries 4:4:4 without an encoder, so an explicit
      // `tiles` request keeps the chroma the viewer asked for.
      out.no_subsampling = true;
    } else if (!caps.stream_444) {
      // No encoder here carries 4:4:4. Dropping to tiles would silently cost
      // the video stream, so resolve to 4:2:0 instead: the tier keeps its QP,
      // bitrate and frame rate and streams.
      out.chroma = "420";
    }
  }
  // `tiles` is the viewer explicitly asking for the pixel-exact path. Keep
  // the 4:2:0 stream off even when the chroma field alone would have allowed
  // it; the supervisor uses this per-session flag.
  if (out.codec == "tiles") out.no_subsampling = true;
  return true;
}

}  // namespace molink
