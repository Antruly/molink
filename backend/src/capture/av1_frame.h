#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "capture/dirty_rects.h"
#include "protocol/frame_packet.h"

namespace molink {

class av1_encoder;

/// Builds one AV1 FRAME payload from the given screen rectangles. Every tile
/// carries codec::av1 and is a standalone intra stream, so the receiver can
/// decode each rectangle independently. `header` supplies frame_seq and the
/// screen geometry; codec and flags are replaced.
bool build_av1_frame_payload(const protocol::frame_header& header,
                             av1_encoder& encoder, const uint8_t* bgra,
                             std::size_t stride,
                             const std::vector<pixel_rect>& rects, int qp,
                             std::vector<uint8_t>& payload,
                             std::string& error);

/// Decodes an AV1 FRAME payload into raw BGRA tiles with the same rectangles.
/// The client core uses this before forwarding to a page that cannot decode
/// AV1 itself. Returns false when this build has no dav1d, the payload is not
/// AV1, or a rectangle fails to decode.
bool decode_av1_frame_payload(const uint8_t* payload, std::size_t len,
                              std::vector<protocol::tile_blob>& tiles,
                              std::string& error);

/// True when this build links dav1d and can decode AV1 rectangles.
bool av1_decode_available();

}  // namespace molink
