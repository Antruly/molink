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

/// Compress tiles (when enabled and worthwhile) and build the FRAME payload.
/// Updates `header.codec` to deflate_bgra only when every tile is compressed
/// and the result is actually smaller; otherwise the frame stays raw BGRA.
bool encode_capture_frame(protocol::frame_header& header,
                          std::vector<protocol::tile_blob>& tiles, bool compress,
                          std::vector<uint8_t>& payload, std::string& err);

}  // namespace molink
