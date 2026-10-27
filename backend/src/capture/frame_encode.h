#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "capture/av1_encoder.h"
#include "protocol/frame_packet.h"

namespace molink {

/// Optional damage rectangle for a partial grab. Coordinates are screen pixels.
struct tile_dirty_rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool valid = false;
};

/// Wire layout used for tile pixels. bgra is lossless; rgb24 drops the alpha
/// channel and rgb565 additionally drops colour precision. The lossy formats
/// are what make the "smooth" preset cheap: they cut the bytes zlib has to
/// push through and the bytes the browser has to inflate, before any entropy
/// coding happens.
enum class tile_format : uint8_t {
  bgra,
  rgb24,
  rgb565,
  /// One luminance byte per pixel. Used by the turbo preset for very poor
  /// links: half the bytes of rgb565 and far less entropy.
  gray8,
};

/// How much CPU the encoder may spend on zlib. `fast` is for low-latency
/// frames, `best` trades CPU for a smaller packet in quality mode, and
/// `balanced` keeps the original adaptive policy.
enum class compression_effort : uint8_t {
  fast,
  balanced,
  best,
};

class av1_encoder;

struct encode_options {
  bool compress = true;
  tile_format format = tile_format::bgra;
  compression_effort effort = compression_effort::balanced;
  /// When set, changed rectangles are encoded as AV1. In full-frame mode every
  /// changed rectangle is AV1 and the tile payload is emptied; otherwise only
  /// large rectangles become a patch and are removed from the tile payload.
  /// The caller must pass the current screen pixels.
  av1_encoder* av1 = nullptr;
  /// AV1 carries the whole picture (tiles are only a fallback).
  bool av1_full_frame = false;
  int av1_qp = 32;
  /// Encode the rectangles as luminance-only (the turbo preset).
  bool av1_grayscale = false;
  /// Encode screen-sized rectangles at half resolution when the host cannot
  /// keep up (see build_av1_frame_payload); the receiver scales them back.
  bool av1_downscale = false;
  /// Box-average large tile strips in place when nearly the whole screen is
  /// changing: same tile size and format on the wire, far fewer bytes.
  bool tile_downscale = false;
  /// Cap on the AV1-encoded area per frame, percent of the screen (100 = no
  /// cap). The uncapped rectangles stay in the tile payload. Ignored in
  /// full-frame mode, which has to encode every changed rectangle.
  int av1_max_area_percent = 100;
  int av1_max_rects = 4;
  /// When set, the whole screen is encoded as one frame of a low-delay AV1
  /// stream (P frames after the first) instead of tiles or patches. The
  /// receiver must keep a matching decoder; see av1_stream_encoder.
  av1_stream_encoder* av1_stream = nullptr;
  /// Force this stream frame to be a keyframe (the first frame after a switch
  /// or after a decode failure on the viewer).
  bool av1_stream_keyframe = false;
  /// Shared hysteresis owned by the capture: while *this monotonic time has
  /// not passed, a near-full-screen change keeps the single full-screen AV1
  /// picture. Video pauses for a frame or two all the time; without the
  /// hold-off the encoder flapped between full-screen and strips, which reads
  /// as a stutter. Null disables the hold-off.
  uint64_t* av1_full_screen_until_us = nullptr;
  /// Minimum rectangle area in pixels; 0 means two capture tiles.
  int av1_min_area = 0;
};

/// Splits @p pixels into changed tiles and coalesces horizontally adjacent
/// changed tiles into strips. A keyframe becomes one full-width strip per tile
/// row, which cuts per-tile headers and gives zlib a larger, better-ratio
/// window. @p previous is updated in place with everything emitted, so the next
/// delta compares against exactly what the client has seen. When
/// @p force_opaque is set, the alpha byte of every emitted pixel is set to 255
/// (macOS CoreGraphics leaves it undefined).
void build_changed_tiles(const uint8_t* pixels, std::size_t stride,
                         uint16_t width, uint16_t height, uint16_t tile_size,
                         uint8_t* previous, bool keyframe,
                         const tile_dirty_rect* dirty, bool force_opaque,
                         int change_threshold,
                         std::vector<protocol::tile_blob>& tiles);

/// Convert tiles to the requested wire format, compress them when worthwhile,
/// and build the FRAME payload. `header.codec` is set to the codec that was
/// actually used: bgra/rgb24/rgb565 for raw tiles, or the matching deflate_*
/// codec when deflate made the frame smaller. A lossy format is never reverted
/// to bgra just because zlib did not help; the packed form is still smaller.
/// `pixels`/`stride` describe the current screen (BGRA) and are only read when
/// `options.av1` is set. In full-frame mode every changed rectangle is encoded
/// into @p av1_payload, @p tiles is cleared and @p av1_only is set; otherwise
/// the covered rectangles are removed from @p tiles and the patch is returned
/// separately. @p av1_payload may be null to disable AV1 for this call.
bool encode_capture_frame(protocol::frame_header& header,
                          std::vector<protocol::tile_blob>& tiles,
                          const encode_options& options, const uint8_t* pixels,
                          std::size_t stride, std::vector<uint8_t>& payload,
                          std::vector<uint8_t>* av1_payload,
                          std::string& err, bool* av1_only = nullptr);

}  // namespace molink
