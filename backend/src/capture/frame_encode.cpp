#include "capture/frame_encode.h"

#include <cstddef>
#include <utility>
#include <vector>

#include "compression.h"

namespace molink {

bool encode_capture_frame(protocol::frame_header& header,
                          std::vector<protocol::tile_blob>& tiles, bool compress,
                          std::vector<uint8_t>& payload, std::string& err) {
  header.codec = static_cast<uint8_t>(protocol::codec::bgra);

  if (compress && compression_available() && !tiles.empty()) {
    std::size_t raw_bytes = 0;
    std::size_t packed_bytes = 0;
    bool all_ok = true;
    std::vector<std::vector<uint8_t>> packed(tiles.size());

    for (std::size_t i = 0; i < tiles.size(); ++i) {
      raw_bytes += tiles[i].data.size();
      if (!deflate_buffer(tiles[i].data.data(), tiles[i].data.size(),
                          packed[i], err)) {
        all_ok = false;
        break;
      }
      packed_bytes += packed[i].size();
    }

    // Compression has a CPU cost on both ends; skip it when the screen is
    // already incompressible (photos, already-encoded video).
    if (all_ok && packed_bytes < raw_bytes) {
      for (std::size_t i = 0; i < tiles.size(); ++i) {
        tiles[i].data = std::move(packed[i]);
      }
      header.codec = static_cast<uint8_t>(protocol::codec::deflate_bgra);
    } else {
      err.clear();
    }
  }

  return protocol::build_frame_payload(header, tiles, payload, err);
}

}  // namespace molink
