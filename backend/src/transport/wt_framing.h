#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace molink {

/// Which stream a datagram belongs to. Video and audio share the datagram
/// path but must not share reassembly state: an audio packet that arrives in
/// the middle of a video frame would otherwise tear both apart.
enum class datagram_channel : uint8_t {
  video = 0,
  audio = 1,
};

/// The channel a datagram belongs to (video when the byte is unknown, which is
/// what a peer that predates this field would send).
datagram_channel datagram_channel_of(const uint8_t* data, std::size_t len);

/// Splits one frame into datagram-sized fragments.
///
/// H3 datagrams are unreliable and MTU-sized (about 1200 bytes), so a video
/// frame has to be cut up; the receiver reassembles by frame id. A frame that
/// loses a fragment is dropped -- the keyframe/repair loop already recovers
/// from a missing picture, and retransmitting would defeat the point of
/// sending it unreliably.
struct datagram_fragment {
  std::vector<uint8_t> bytes;
};

/// Largest frame worth fragmenting. A keyframe can be far bigger; it goes on
/// the reliable stream instead, where losing bytes is not fatal.
inline constexpr std::size_t kDatagramFrameLimit = 256 * 1024;

/// Datagram payload budget: QUIC datagrams must fit one MTU with the H3
/// datagram frame header on top.
inline constexpr std::size_t kDatagramPayloadBytes = 1100;

/// Fragments @p data into one datagram per chunk. Returns an empty list when
/// the frame is too large to send as datagrams.
std::vector<datagram_fragment> fragment_frame(datagram_channel channel,
                                              const uint8_t* data,
                                              std::size_t len,
                                              uint32_t frame_id);

/// Reassembles one frame from its datagrams. Out-of-order fragments are kept
/// until the frame is complete or a newer frame id arrives.
class datagram_reassembler {
 public:
  /// Feeds one datagram. Returns true and fills @p out when it completed the
  /// frame that was being assembled.
  bool push(const uint8_t* data, std::size_t len, std::vector<uint8_t>& out);

  /// Drops the partial frame (a new frame id always does this implicitly).
  void reset();

  /// Frames dropped because the next one started before they completed, or
  /// because they were malformed. Diagnostics only.
  std::size_t dropped() const { return dropped_; }

 private:
  uint32_t frame_id_ = 0;
  uint16_t count_ = 0;
  bool active_ = false;
  std::size_t bytes_ = 0;
  std::map<uint16_t, std::vector<uint8_t>> parts_;
  std::size_t dropped_ = 0;
};

}  // namespace molink
