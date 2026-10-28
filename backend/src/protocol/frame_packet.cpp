#include "protocol/frame_packet.h"

#include <cstring>

namespace molink {
namespace protocol {
namespace {

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xffu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
}

void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xffu);
}

void put64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xffu);
}

uint16_t get16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t get32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}

uint64_t get64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

bool ensure_room(std::size_t have, std::size_t need, const char* what,
                 std::string& err) {
  if (have < need) {
    err = std::string("truncated ") + what + ": need " + std::to_string(need) +
          " bytes, have " + std::to_string(have);
    return false;
  }
  return true;
}

}  // namespace

bool encode_packet(const packet& p, std::vector<uint8_t>& out, std::string& err) {
  return encode_packet_into(out, p.type, p.flags, p.session, p.seq, p.ts_us,
                            p.payload.data(), p.payload.size(), err);
}

bool encode_packet_into(std::vector<uint8_t>& out, uint8_t type, uint16_t flags,
                        uint32_t session, uint32_t seq, uint64_t ts_us,
                        const uint8_t* payload, std::size_t payload_len,
                        std::string& err) {
  if (payload_len > kMaxPayloadBytes) {
    err = "payload exceeds " + std::to_string(kMaxPayloadBytes) + " bytes";
    return false;
  }
  if (payload_len != 0 && payload == nullptr) {
    err = "payload is null";
    return false;
  }

  out.resize(kPacketHeaderSize + payload_len);
  std::memcpy(out.data(), kMagic, sizeof(kMagic));
  out[4] = kProtocolVersion;
  out[5] = type;
  put16(out.data() + 6, flags);
  put32(out.data() + 8, session);
  put32(out.data() + 12, seq);
  put64(out.data() + 16, ts_us);
  put32(out.data() + 24, static_cast<uint32_t>(payload_len));
  put32(out.data() + 28, 0);
  if (payload_len != 0) {
    std::memcpy(out.data() + kPacketHeaderSize, payload, payload_len);
  }
  return true;
}

bool decode_packet(const uint8_t* data, std::size_t len, packet& out,
                   std::string& err) {
  if (!ensure_room(len, kPacketHeaderSize, "packet header", err)) return false;
  if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
    err = "bad magic (expected MOLK)";
    return false;
  }

  const uint8_t version = data[4];
  if (version != kProtocolVersion) {
    err = "unsupported protocol version " + std::to_string(version);
    return false;
  }

  const uint32_t payload_len = get32(data + 24);
  if (payload_len > kMaxPayloadBytes) {
    err = "payload length exceeds limit: " + std::to_string(payload_len);
    return false;
  }
  if (len != kPacketHeaderSize + static_cast<std::size_t>(payload_len)) {
    err = "payload length mismatch: header says " +
          std::to_string(payload_len) + ", buffer has " +
          std::to_string(len - kPacketHeaderSize);
    return false;
  }

  out.version = version;
  out.type = data[5];
  out.flags = get16(data + 6);
  out.session = get32(data + 8);
  out.seq = get32(data + 12);
  out.ts_us = get64(data + 16);
  out.payload.assign(data + kPacketHeaderSize,
                     data + kPacketHeaderSize + payload_len);
  return true;
}

bool build_frame_payload(const frame_header& h,
                         const std::vector<tile_blob>& tiles,
                         std::vector<uint8_t>& out, std::string& err) {
  if (tiles.size() > kMaxTilesPerFrame) {
    err = "too many tiles: " + std::to_string(tiles.size());
    return false;
  }

  std::size_t total = kFrameHeaderSize + tiles.size() * kTileEntrySize;
  for (const tile_blob& t : tiles) {
    if (t.data.size() > kMaxPayloadBytes ||
        total > kMaxPayloadBytes - t.data.size()) {
      err = "frame payload exceeds " + std::to_string(kMaxPayloadBytes) +
            " bytes";
      return false;
    }
    total += t.data.size();
  }

  out.assign(total, 0);
  uint8_t* p = out.data();
  put32(p + 0, h.frame_seq);
  put16(p + 4, static_cast<uint16_t>(tiles.size()));
  p[6] = h.codec;
  p[7] = h.flags;
  put16(p + 8, h.screen_w);
  put16(p + 10, h.screen_h);
  put16(p + 12, h.tile_w);
  put16(p + 14, h.tile_h);

  uint8_t* entry = p + kFrameHeaderSize;
  uint8_t* body = entry + tiles.size() * kTileEntrySize;
  for (const tile_blob& t : tiles) {
    put16(entry + 0, t.x);
    put16(entry + 2, t.y);
    put16(entry + 4, t.w);
    put16(entry + 6, t.h);
    entry[8] = t.flags;
    put32(entry + 12, static_cast<uint32_t>(t.data.size()));
    if (!t.data.empty()) {
      std::memcpy(body, t.data.data(), t.data.size());
      body += t.data.size();
    }
    entry += kTileEntrySize;
  }
  return true;
}

bool parse_frame_payload(const uint8_t* data, std::size_t len,
                         parsed_frame& out, std::string& err) {
  if (!ensure_room(len, kFrameHeaderSize, "frame header", err)) return false;

  out.header.frame_seq = get32(data + 0);
  out.header.tile_count = get16(data + 4);
  out.header.codec = data[6];
  out.header.flags = data[7];
  out.header.screen_w = get16(data + 8);
  out.header.screen_h = get16(data + 10);
  out.header.tile_w = get16(data + 12);
  out.header.tile_h = get16(data + 14);

  const std::size_t count = out.header.tile_count;
  if (count > kMaxTilesPerFrame) {
    err = "too many tiles: " + std::to_string(count);
    return false;
  }
  if (count > (len - kFrameHeaderSize) / kTileEntrySize) {
    err = "truncated tile table: " + std::to_string(count) + " entries";
    return false;
  }

  const uint8_t* entry = data + kFrameHeaderSize;
  const uint8_t* body = entry + count * kTileEntrySize;
  const uint8_t* end = data + len;

  out.tiles.clear();
  out.tiles.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    tile_view v;
    v.x = get16(entry + 0);
    v.y = get16(entry + 2);
    v.w = get16(entry + 4);
    v.h = get16(entry + 6);
    v.flags = entry[8];
    v.size = get32(entry + 12);
    if (v.size > static_cast<std::size_t>(end - body)) {
      err = "tile " + std::to_string(i) + " overruns frame payload";
      return false;
    }
    v.data = body;
    body += v.size;
    out.tiles.push_back(v);
    entry += kTileEntrySize;
  }

  if (body != end) {
    err = "frame payload has " + std::to_string(end - body) +
          " trailing bytes";
    return false;
  }
  return true;
}

bool has_packet_header(const uint8_t* data, std::size_t len) {
  return data != nullptr && len >= kPacketHeaderSize &&
         std::memcmp(data, kMagic, sizeof(kMagic)) == 0;
}

packet_stream packet_stream_of(const uint8_t* data, std::size_t len) {
  if (!has_packet_header(data, len)) return packet_stream::control;
  switch (static_cast<packet_type>(data[5])) {
    case packet_type::frame:
    case packet_type::tile_clock:
    case packet_type::tile_seq:
      return packet_stream::video;
    case packet_type::audio:
    case packet_type::voice:
      return packet_stream::audio;
    case packet_type::file_data:
      return packet_stream::file;
    default:
      return packet_stream::control;
  }
}

bool packet_is_keyframe(const uint8_t* data, std::size_t len) {
  if (!has_packet_header(data, len)) return false;
  if (data[5] != static_cast<uint8_t>(packet_type::frame)) return false;
  if ((get16(data + 6) & kPacketFlagKeyframe) != 0) return true;
  const std::size_t frame_header_offset = kPacketHeaderSize + 7;
  if (len <= frame_header_offset) return false;
  return (data[frame_header_offset] & kFrameFlagKeyframe) != 0;
}

}  // namespace protocol
}  // namespace molink
