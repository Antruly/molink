#include "transport/juice_transport.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

namespace molink {
namespace {

constexpr uint8_t kTextTag = 0x00;
constexpr uint8_t kBinaryTag = 0x01;
constexpr uint8_t kFragmentTag = 0x02;
// Stay below a typical 1500-byte path MTU after the IP/UDP headers and the
// ICE/TURN encapsulation.
constexpr std::size_t kMaxDatagram = 1200;
constexpr std::size_t kFragmentHeader = 10;  // tag, kind, id, index, count
constexpr std::size_t kFragmentChunk = kMaxDatagram - kFragmentHeader;
constexpr std::size_t kMaxPendingMessages = 16;

uint32_t read_u32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t read_u16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

void write_u32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

void write_u16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xff);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

}  // namespace

juice_transport::juice_transport(std::shared_ptr<ice_agent> agent,
                                 std::string peer_ip, unsigned int peer_port)
    : agent_(std::move(agent)),
      peer_ip_(std::move(peer_ip)),
      peer_port_(peer_port) {
  if (agent_) {
    agent_->set_data_handler(
        [this](const uint8_t* data, std::size_t len) {
          handle_datagram(data, len);
        });
  }
}

juice_transport::~juice_transport() {
  if (agent_) {
    agent_->set_data_handler(nullptr);
    agent_->close();
  }
}

bool juice_transport::is_open() const {
  return !closed_ && agent_ &&
         agent_->state() == static_cast<int>(JUICE_STATE_COMPLETED);
}

bool juice_transport::send_text(const std::string& text) {
  return send_message(kTextTag, reinterpret_cast<const uint8_t*>(text.data()),
                      text.size());
}

bool juice_transport::send_binary(const uint8_t* data, std::size_t len) {
  return send_message(kBinaryTag, data, len);
}

bool juice_transport::send_message(uint8_t kind, const uint8_t* data,
                                   std::size_t len) {
  if (!is_open() || (data == nullptr && len > 0)) return false;
  if (len + 1 <= kMaxDatagram) {
    std::vector<uint8_t> datagram;
    datagram.reserve(len + 1);
    datagram.push_back(kind == kBinaryTag ? kBinaryTag : kTextTag);
    if (len > 0) datagram.insert(datagram.end(), data, data + len);
    return agent_->send(datagram.data(), datagram.size());
  }

  const std::size_t count = (len + kFragmentChunk - 1) / kFragmentChunk;
  if (count == 0 || count > 0xFFFFu) return false;
  const uint32_t id = next_message_id_++;
  bool ok = true;
  for (std::size_t index = 0; index < count; ++index) {
    const std::size_t offset = index * kFragmentChunk;
    const std::size_t remaining = len - offset;
    const std::size_t size =
        remaining < kFragmentChunk ? remaining : kFragmentChunk;
    std::vector<uint8_t> datagram(kFragmentHeader + size);
    datagram[0] = kFragmentTag;
    datagram[1] = kind == kBinaryTag ? kBinaryTag : kTextTag;
    write_u32(datagram.data() + 2, id);
    write_u16(datagram.data() + 6, static_cast<uint16_t>(index));
    write_u16(datagram.data() + 8, static_cast<uint16_t>(count));
    std::memcpy(datagram.data() + kFragmentHeader, data + offset, size);
    if (!agent_->send(datagram.data(), datagram.size())) ok = false;
  }
  return ok;
}

void juice_transport::close(int code, const std::string& reason) {
  if (closed_) return;
  closed_ = true;
  if (agent_) agent_->close();
  if (close_) {
    close_handler handler = std::move(close_);
    text_ = nullptr;
    binary_ = nullptr;
    handler(code, reason);
  }
}

void juice_transport::set_handlers(text_handler on_text,
                                   binary_handler on_binary,
                                   close_handler on_close) {
  text_ = std::move(on_text);
  binary_ = std::move(on_binary);
  close_ = std::move(on_close);
}

void juice_transport::handle_datagram(const uint8_t* data, std::size_t len) {
  if (closed_ || data == nullptr || len < 1) return;
  const uint8_t tag = data[0];
  if (tag == kTextTag || tag == kBinaryTag) {
    const uint8_t* payload = data + 1;
    const std::size_t size = len - 1;
    if (tag == kTextTag) {
      if (text_) text_(std::string(reinterpret_cast<const char*>(payload), size));
    } else if (binary_) {
      binary_(payload, size);
    }
    return;
  }
  if (tag != kFragmentTag || len < kFragmentHeader) return;

  const uint8_t kind = data[1];
  const uint32_t id = read_u32(data + 2);
  const uint16_t index = read_u16(data + 6);
  const uint16_t count = read_u16(data + 8);
  if (count == 0 || index >= count) return;
  const uint8_t* payload = data + kFragmentHeader;
  const std::size_t size = len - kFragmentHeader;

  auto it = fragments_.find(id);
  if (it == fragments_.end()) {
    if (fragments_.size() >= kMaxPendingMessages) {
      fragments_.erase(fragments_.begin());
    }
    fragment_set entry;
    entry.kind = kind;
    entry.count = count;
    entry.parts.resize(count);
    entry.received.assign(count, false);
    it = fragments_.emplace(id, std::move(entry)).first;
  }
  fragment_set& entry = it->second;
  if (entry.count != count || entry.kind != kind) return;
  if (index >= entry.parts.size()) return;
  if (!entry.received[index]) {
    entry.parts[index].assign(reinterpret_cast<const char*>(payload), size);
    entry.received[index] = true;
    ++entry.received_count;
  }
  if (entry.received_count < entry.count) return;

  std::string message;
  for (const std::string& part : entry.parts) message += part;
  const uint8_t message_kind = entry.kind;
  fragments_.erase(it);
  if (message_kind == kTextTag) {
    if (text_) text_(message);
  } else if (binary_) {
    binary_(reinterpret_cast<const uint8_t*>(message.data()), message.size());
  }
}

void juice_transport::notify_closed(int code, const std::string& reason) {
  close(code, reason);
}

}  // namespace molink
