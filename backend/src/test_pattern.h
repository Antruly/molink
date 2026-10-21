#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace molink {

/// Synthetic BGRA frame used until real screen capture lands. It exercises the
/// whole path (tile grid -> FRAME payload -> MOLK packet -> WebSocket binary ->
/// browser decode) so the protocol can be verified before any platform capture
/// backend exists.
struct test_pattern_options {
  uint16_t width = 640;
  uint16_t height = 360;
  uint16_t tile = 128;

  /// Largest total frame payload this source will produce.
  static constexpr std::size_t kMaxBytes = 4u * 1024u * 1024u;
};

bool build_test_frame_packet(uint32_t session, uint32_t seq, uint64_t ts_us,
                             const test_pattern_options& opts,
                             std::vector<uint8_t>& out, std::string& err);

}  // namespace molink
