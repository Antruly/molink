#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol/frame_packet.h"

namespace molink {

/// Compress tiles (when enabled and worthwhile) and build the FRAME payload.
/// Updates `header.codec` to deflate_bgra only when every tile is compressed
/// and the result is actually smaller; otherwise the frame stays raw BGRA.
bool encode_capture_frame(protocol::frame_header& header,
                          std::vector<protocol::tile_blob>& tiles, bool compress,
                          std::vector<uint8_t>& payload, std::string& err);

}  // namespace molink
