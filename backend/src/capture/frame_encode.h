#pragma once

#include <cstdint>
#include <string>
#include <vector>

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
};

/// How much CPU the encoder may spend on zlib. `fast` is for low-latency
/// frames, `best` trades CPU for a smaller packet in quality mode, and
/// `balanced` keeps the original adaptive policy.
enum class compression_effort : uint8_t {
  fast,
  balanced,
  best,
};

struct encode_options {
  bool compress = true;
  tile_format format = tile_format::bgra;
  compression_effort effort = compression_effort::balanced;
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
bool encode_capture_frame(protocol::frame_header& header,
                          std::vector<protocol::tile_blob>& tiles,
                          const encode_options& options,
                          std::vector<uint8_t>& payload, std::string& err);

}  // namespace molink
