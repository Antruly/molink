#include "transport/wt_framing.h"

#include <cstring>

namespace molink {
namespace {

constexpr std::size_t kFragmentHeader = 9;

void put32(uint8_t* out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffu);
  }
}

uint32_t get32(const uint8_t* in) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(in[i]) << (8 * i);
  }
  return value;
}

void put16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xffu);
  out[1] = static_cast<uint8_t>(value >> 8);
}

uint16_t get16(const uint8_t* in) {
  return static_cast<uint16_t>(in[0] | (static_cast<uint16_t>(in[1]) << 8));
}

}  // namespace

datagram_channel datagram_channel_of(const uint8_t* data, std::size_t len) {
  if (data == nullptr || len < kFragmentHeader) return datagram_channel::video;
  return data[0] == static_cast<uint8_t>(datagram_channel::audio)
             ? datagram_channel::audio
             : datagram_channel::video;
}

std::vector<datagram_fragment> fragment_frame(datagram_channel channel,
                                              const uint8_t* data,
                                              std::size_t len,
                                              uint32_t frame_id) {
  std::vector<datagram_fragment> fragments;
  if (data == nullptr || len == 0 || len > kDatagramFrameLimit) return fragments;
  const std::size_t chunk = kDatagramPayloadBytes - kFragmentHeader;
  const std::size_t count = (len + chunk - 1) / chunk;
  if (count == 0 || count > 0xffffu) return fragments;

  fragments.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t offset = index * chunk;
    const std::size_t size = (len - offset) < chunk ? (len - offset) : chunk;
    datagram_fragment fragment;
    fragment.bytes.resize(kFragmentHeader + size);
    fragment.bytes[0] = static_cast<uint8_t>(channel);
    put32(fragment.bytes.data() + 1, frame_id);
    put16(fragment.bytes.data() + 5, static_cast<uint16_t>(index));
    put16(fragment.bytes.data() + 7, static_cast<uint16_t>(count));
    std::memcpy(fragment.bytes.data() + kFragmentHeader, data + offset, size);
    fragments.push_back(std::move(fragment));
  }
  return fragments;
}

bool datagram_reassembler::push(const uint8_t* data, std::size_t len,
                                std::vector<uint8_t>& out) {
  if (data == nullptr || len <= kFragmentHeader) return false;
  const uint32_t frame_id = get32(data + 1);
  const uint16_t index = get16(data + 5);
  const uint16_t count = get16(data + 7);
  if (count == 0 || index >= count) return false;

  if (!active_ || frame_id != frame_id_) {
    // A newer frame supersedes whatever was still incomplete: the picture it
    // belonged to is already stale.
    if (active_ && !parts_.empty()) ++dropped_;
    parts_.clear();
    bytes_ = 0;
    frame_id_ = frame_id;
    count_ = count;
    active_ = true;
  }
  if (count_ != count) return false;
  if (parts_.find(index) != parts_.end()) return false;  // duplicate

  const std::size_t payload = len - kFragmentHeader;
  if (bytes_ + payload > kDatagramFrameLimit) {
    reset();
    return false;
  }
  bytes_ += payload;
  parts_.emplace(index, std::vector<uint8_t>(data + kFragmentHeader,
                                             data + len));
  if (parts_.size() != count_) return false;

  out.clear();
  out.reserve(bytes_);
  for (const auto& part : parts_) {
    out.insert(out.end(), part.second.begin(), part.second.end());
  }
  reset();
  return true;
}

void datagram_reassembler::reset() {
  parts_.clear();
  bytes_ = 0;
  count_ = 0;
  active_ = false;
  frame_id_ = 0;
}

}  // namespace molink
