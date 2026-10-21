/**
 * @file frame_packet.h
 * @brief moLink wire protocol: the "MOLK" packet and its FRAME payload.
 *
 * This header is the single source of truth for the bytes that cross a
 * transport. Both transports — custom UDP between desktop cores and WebRTC
 * RTCDataChannel between a browser and a core — carry exactly these bytes.
 * Nothing here may depend on libuvcpp, so the codec can be reused by a future
 * UDP path, a test harness, or a second implementation in another language.
 *
 * The "MOLK" magic is a protocol contract (see docs/dev.md §0) and must never
 * change. kProtocolVersion may be bumped for layout changes; every field is
 * little-endian regardless of host byte order.
 *
 * Packet header (32 bytes):
 *
 *   offset  size  field
 *   0       4     magic       "MOLK"
 *   4       1     version     kProtocolVersion
 *   5       1     type        packet_type
 *   6       2     flags       packet_flags
 *   8       4     session     session id assigned by the core
 *   12      4     seq         packet sequence within the session
 *   16      8     ts_us       capture timestamp, microseconds since Unix epoch
 *   24      4     payload_len length of the payload that follows
 *   28      4     reserved    zero
 *
 * FRAME payload (16-byte body header followed by N tile entries and tile bytes):
 *
 *   offset  size  field
 *   0       4     frame_seq
 *   4       2     tile_count
 *   6       1     codec       codec enum
 *   7       1     flags       frame_flags
 *   8       2     screen_w
 *   10      2     screen_h
 *   12      2     tile_w      nominal tile grid width
 *   14      2     tile_h      nominal tile grid height
 *
 *   then tile_count entries of 16 bytes:
 *   0       2     x
 *   2       2     y
 *   4       2     w
 *   6       2     h
 *   8       1     flags       tile_flags
 *   9       3     reserved    zero
 *   12      4     byte_length
 *
 *   then the tile payloads, concatenated in entry order. Each tile's bytes are
 *   codec-specific: raw BGRA rows for codec::bgra, an encoded bitstream for
 *   codec::h264 / codec::av1. A tile's encoded bytes never span two packets.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molink {
namespace protocol {

inline constexpr char kMagic[4] = {'M', 'O', 'L', 'K'};
inline constexpr std::size_t kPacketHeaderSize = 32;
inline constexpr std::size_t kFrameHeaderSize = 16;
inline constexpr std::size_t kTileEntrySize = 16;
inline constexpr uint8_t kProtocolVersion = 1;

/// Upper bound for one packet payload. Keeps a malformed or hostile length
/// field from asking the receiver to allocate the world. A 4K BGRA keyframe is
/// about 33 MiB, so this must stay above that.
inline constexpr uint32_t kMaxPayloadBytes = 64u * 1024u * 1024u;
inline constexpr uint16_t kMaxTilesPerFrame = 4096;

enum class packet_type : uint8_t {
  hello = 0x01,      ///< JSON control payload (transport-level hello)
  frame = 0x02,      ///< FRAME payload, see build_frame_payload()
  ack = 0x03,        ///< JSON control payload
  input = 0x10,      ///< JSON input event batch (pointer/key)
  clipboard = 0x11,  ///< JSON clipboard payload
  control = 0x7f,    ///< JSON control payload, session-scoped
  ping = 0x80,       ///< empty payload
  pong = 0x81,       ///< empty payload
};

enum packet_flags : uint16_t {
  kPacketFlagKeyframe = 1u << 0,
  kPacketFlagCompressed = 1u << 1,
  kPacketFlagLast = 1u << 2,
};

enum class codec : uint8_t {
  bgra = 1,            ///< raw 32-bit BGRA tiles, top-down
  png = 2,             ///< PNG-encoded tile
  h264 = 3,            ///< H.264 Annex-B tile
  av1 = 4,             ///< AV1 OBU tile
  deflate_bgra = 5,    ///< zlib deflate of a raw BGRA tile
  rgb24 = 6,           ///< packed 24-bit RGB (R,G,B), alpha dropped
  rgb565 = 7,          ///< packed 16-bit RGB565, little-endian
  deflate_rgb24 = 8,   ///< zlib deflate of an rgb24 tile
  deflate_rgb565 = 9,  ///< zlib deflate of an rgb565 tile
};

enum frame_flags : uint8_t {
  kFrameFlagKeyframe = 1u << 0,
  kFrameFlagPartial = 1u << 1,  ///< only changed tiles are present
};

enum tile_flags : uint8_t {
  kTileFlagChanged = 1u << 0,
  kTileFlagKeyframe = 1u << 1,
};

struct packet {
  uint8_t version = kProtocolVersion;
  uint8_t type = 0;  ///< packet_type
  uint16_t flags = 0;
  uint32_t session = 0;
  uint32_t seq = 0;
  uint64_t ts_us = 0;
  std::vector<uint8_t> payload;
};

/// Serialize one packet. Returns false and fills @p err on invalid input.
bool encode_packet(const packet& p, std::vector<uint8_t>& out, std::string& err);

/// Serialize a packet without first copying the payload into a packet object.
/// Used on the frame hot path, where the payload is already a contiguous
/// buffer produced by the capture thread.
bool encode_packet_into(std::vector<uint8_t>& out, uint8_t type, uint16_t flags,
                        uint32_t session, uint32_t seq, uint64_t ts_us,
                        const uint8_t* payload, std::size_t payload_len,
                        std::string& err);

/// Parse exactly one packet from [data, data + len). The buffer must contain
/// one whole packet and nothing after it. Returns false and fills @p err on
/// bad magic, unsupported version, or a payload length that does not match.
bool decode_packet(const uint8_t* data, std::size_t len, packet& out,
                   std::string& err);

struct frame_header {
  uint32_t frame_seq = 0;
  uint16_t tile_count = 0;
  uint8_t codec = 0;  ///< codec
  uint8_t flags = 0;  ///< frame_flags
  uint16_t screen_w = 0;
  uint16_t screen_h = 0;
  uint16_t tile_w = 0;
  uint16_t tile_h = 0;
};

struct tile_blob {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  uint8_t flags = 0;  ///< tile_flags
  std::vector<uint8_t> data;
};

/// Build a FRAME payload from tiles. Fills in tile_count on the copy of the
/// header that goes on the wire; @p h itself is not modified. Tile data larger
/// than kMaxPayloadBytes is rejected.
bool build_frame_payload(const frame_header& h,
                         const std::vector<tile_blob>& tiles,
                         std::vector<uint8_t>& out, std::string& err);

struct tile_view {
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  uint8_t flags = 0;
  const uint8_t* data = nullptr;
  std::size_t size = 0;
};

struct parsed_frame {
  frame_header header;
  std::vector<tile_view> tiles;
};

/// Parse a FRAME payload. Tile views point into @p data and stay valid as long
/// as the caller owns that buffer.
bool parse_frame_payload(const uint8_t* data, std::size_t len,
                         parsed_frame& out, std::string& err);

}  // namespace protocol
}  // namespace molink
