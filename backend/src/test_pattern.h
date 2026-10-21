#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace molink {

/// Animated BGRA test pattern. Used as the synthetic capture source, and as
/// the fallback when no display is reachable, so the protocol and renderer can
/// be exercised without a screen.
struct test_pattern_options {
  uint16_t width = 640;
  uint16_t height = 360;
  uint16_t tile = 128;

  /// Largest total frame payload this source will produce.
  static constexpr std::size_t kMaxBytes = 4u * 1024u * 1024u;
};

/// Builds a FRAME payload for frame @p frame_seq. Every 60th frame is marked
/// as a keyframe, so both the keyframe and delta paths get exercised.
bool build_test_frame_payload(uint32_t frame_seq,
                              const test_pattern_options& opts,
                              std::vector<uint8_t>& out, bool& keyframe,
                              std::string& err);

}  // namespace molink
