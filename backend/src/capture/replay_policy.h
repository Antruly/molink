#pragma once

#include <cstdint>

namespace molink {

inline constexpr uint32_t kReplayMaxFrames = 60;
inline constexpr uint64_t kReplayMaxAgeUs = 1000000ULL;

enum class replay_action {
  replay,
  skip_to_latest,
  request_keyframe,
};

inline replay_action choose_replay_action(uint32_t missing_frames,
                                          uint64_t oldest_frame_age_us,
                                          bool has_image) {
  const bool exceeded = missing_frames > kReplayMaxFrames ||
                        oldest_frame_age_us > kReplayMaxAgeUs;
  if (!exceeded) return replay_action::replay;
  return has_image ? replay_action::skip_to_latest
                   : replay_action::request_keyframe;
}

}  // namespace molink
