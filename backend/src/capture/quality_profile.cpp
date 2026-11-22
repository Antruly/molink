// Pure quality-preset helpers. Kept out of capture.cpp so policy/validation
// tests can link them without a capture backend (and its codec libraries).
#include "capture/capture.h"

namespace molink {

const char* quality_mode_name(quality_mode mode) {
  switch (mode) {
    case quality_mode::smooth:
      return "smooth";
    case quality_mode::quality:
      return "quality";
    case quality_mode::turbo:
      return "turbo";
    case quality_mode::raw:
      return "raw";
    case quality_mode::ultra:
      return "ultra";
    case quality_mode::lossless:
      return "lossless";
    case quality_mode::automatic:
      return "auto";
    case quality_mode::balanced:
    default:
      return "balanced";
  }
}

bool quality_mode_from_name(const std::string& name, quality_mode& out) {
  if (name == "smooth") {
    out = quality_mode::smooth;
    return true;
  }
  if (name == "balanced") {
    out = quality_mode::balanced;
    return true;
  }
  if (name == "quality") {
    out = quality_mode::quality;
    return true;
  }
  if (name == "turbo") {
    out = quality_mode::turbo;
    return true;
  }
  // "超清" and "无损" keep original BGRA pixels with zlib (lossless); the
  // client chooses the frame rate (15 vs 30). "游戏" prioritises frame rate
  // over colour bits. "raw" stays available for clients that want no zlib at
  // all, but the UI does not expose it because a 1080p 30 fps raw stream is
  // far beyond a normal LAN.
  if (name == "raw") {
    out = quality_mode::raw;
    return true;
  }
  if (name == "ultra") {
    out = quality_mode::ultra;
    return true;
  }
  if (name == "lossless") {
    out = quality_mode::lossless;
    return true;
  }
  if (name == "auto") {
    out = quality_mode::automatic;
    return true;
  }
  if (name == "game") {
    out = quality_mode::smooth;
    return true;
  }
  return false;
}

bool tile_format_from_name(const std::string& name, tile_format& out) {
  if (name == "bgra" || name == "truecolor") {
    out = tile_format::bgra;
    return true;
  }
  if (name == "rgb24") {
    out = tile_format::rgb24;
    return true;
  }
  if (name == "rgb565") {
    out = tile_format::rgb565;
    return true;
  }
  if (name == "gray8" || name == "mono") {
    out = tile_format::gray8;
    return true;
  }
  return false;
}

const char* tile_format_name(tile_format format) {
  switch (format) {
    case tile_format::rgb24:
      return "rgb24";
    case tile_format::rgb565:
      return "rgb565";
    case tile_format::gray8:
      return "gray8";
    case tile_format::bgra:
    default:
      return "bgra";
  }
}

quality_profile quality_profile_for(quality_mode mode) {
  quality_profile profile;
  switch (mode) {
    case quality_mode::smooth:
      // 16-bit colour cuts the bytes that must be deflated, inflated and
      // painted; a coarser change threshold drops fine noise as well. The
      // client compensates with a higher capture/pull rate.
      profile.format = tile_format::rgb565;
      profile.effort = compression_effort::fast;
      profile.minimum_change_threshold = 16;
      profile.lossless = false;
      profile.av1_qp = 32;
      profile.av1_max_area_percent = 60;
      profile.stream_bits_per_pixel_x1000 = 25;
      break;
    case quality_mode::quality:
      // Keep every colour bit, spend more CPU on zlib, and never ignore a
      // visual change that the default threshold would have skipped.
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::best;
      profile.lossless = true;
      // Near-lossless but still encodable in real time: QP 12 on a bounded
      // area is much faster than QP 4 over the whole screen.
      profile.av1_qp = 12;
      profile.av1_max_area_percent = 20;
      profile.stream_bits_per_pixel_x1000 = 70;
      profile.maximum_change_threshold = 1;
      break;
    case quality_mode::turbo:
      // Monochrome and coarse: the absolute minimum for a link that cannot
      // move colour frames at all. The client pairs it with a low frame rate.
      profile.format = tile_format::gray8;
      profile.effort = compression_effort::fast;
      profile.minimum_change_threshold = 24;
      profile.lossless = false;
      profile.av1_qp = 36;
      profile.av1_grayscale = true;
      profile.stream_bits_per_pixel_x1000 = 15;
      profile.av1_max_area_percent = 60;
      break;
    case quality_mode::ultra:
      // Above "quality": a sharper starting QP for the stream, still within
      // what a GPU encoder can run in real time.
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::best;
      profile.lossless = true;
      profile.av1_qp = 8;
      profile.av1_max_area_percent = 15;
      // The top of the ladder: constant quality, no bitrate ceiling.
      profile.stream_bits_per_pixel_x1000 = 0;
      profile.maximum_change_threshold = 1;
      break;
    case quality_mode::lossless:
      // The top of the ladder: near-lossless, lowest practical QP.
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::best;
      profile.lossless = true;
      profile.av1_qp = 3;
      profile.av1_max_area_percent = 12;
      profile.stream_bits_per_pixel_x1000 = 0;
      profile.maximum_change_threshold = 1;
      break;
    case quality_mode::automatic:
      // Resolved at runtime by effective_quality(); a direct lookup only
      // happens for the raw preset name, so answer with the middle level.
      return quality_profile_for(quality_mode::balanced);
    case quality_mode::raw:
      // Original BGRA pixels, deliberately not deflated. Useful on a fast
      // local link and for the "lossless" presets where the client trades
      // bandwidth for the least possible encode latency.
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::fast;
      profile.compress = false;
      profile.lossless = true;
      profile.av1_qp = 1;
      profile.av1_max_area_percent = 20;
      profile.stream_bits_per_pixel_x1000 = 0;
      profile.maximum_change_threshold = 1;
      // Undeflated BGRA is the most expensive stream there is; giving AV1 up
      // here would not buy a usable frame rate, only a much larger one.
      profile.av1_fallback_to_tiles = false;
      break;
    case quality_mode::balanced:
    default:
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::balanced;
      profile.av1_qp = 24;
      profile.av1_max_area_percent = 35;
      profile.stream_bits_per_pixel_x1000 = 45;
      break;
  }
  return profile;
}

}  // namespace molink
