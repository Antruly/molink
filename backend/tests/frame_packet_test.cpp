// Protocol codec tests. No framework on purpose: this file is also the
// executable specification for the byte layout, and it must build anywhere
// molink-core builds.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "protocol/frame_packet.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void expect(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  }
}

void test_packet_roundtrip() {
  using namespace molink::protocol;
  packet p;
  p.type = static_cast<uint8_t>(packet_type::control);
  p.flags = kPacketFlagLast;
  p.session = 42;
  p.seq = 7;
  p.ts_us = 1234567890123ull;
  p.payload = {'{', '"', 'x', '"', ':', '1', '}'};

  std::vector<uint8_t> bytes;
  std::string err;
  expect(encode_packet(p, bytes, err), "encode_packet succeeds: " + err);
  expect(bytes.size() == kPacketHeaderSize + p.payload.size(),
         "encoded size is header + payload");
  expect(std::memcmp(bytes.data(), "MOLK", 4) == 0, "magic is MOLK");
  expect(bytes[4] == kProtocolVersion, "version byte is written");

  packet q;
  expect(decode_packet(bytes.data(), bytes.size(), q, err),
         "decode_packet succeeds: " + err);
  expect(q.type == p.type, "type round-trips");
  expect(q.flags == p.flags, "flags round-trip");
  expect(q.session == p.session, "session round-trips");
  expect(q.seq == p.seq, "seq round-trips");
  expect(q.ts_us == p.ts_us, "ts_us round-trips");
  expect(q.payload == p.payload, "payload round-trips");
}

void test_packet_rejects() {
  using namespace molink::protocol;
  packet p;
  p.type = static_cast<uint8_t>(packet_type::ping);
  std::vector<uint8_t> bytes;
  std::string err;
  expect(encode_packet(p, bytes, err), "ping packet encodes");

  {
    std::vector<uint8_t> bad = bytes;
    bad[0] = 'X';
    packet q;
    expect(!decode_packet(bad.data(), bad.size(), q, err),
           "bad magic is rejected");
  }
  {
    std::vector<uint8_t> bad = bytes;
    bad[4] = 99;
    packet q;
    expect(!decode_packet(bad.data(), bad.size(), q, err),
           "unknown version is rejected");
  }
  {
    packet q;
    expect(!decode_packet(bytes.data(), bytes.size() - 1, q, err),
           "truncated buffer is rejected");
  }
  {
    std::vector<uint8_t> bad = bytes;
    // Claim a payload that is not there.
    bad[24] = 1;
    packet q;
    expect(!decode_packet(bad.data(), bad.size(), q, err),
           "payload length mismatch is rejected");
  }
  {
    packet q;
    expect(!decode_packet(bytes.data(), 0, q, err),
           "empty buffer is rejected");
  }
}

void test_frame_roundtrip() {
  using namespace molink::protocol;
  frame_header fh;
  fh.frame_seq = 9;
  fh.codec = static_cast<uint8_t>(codec::bgra);
  fh.flags = kFrameFlagKeyframe;
  fh.screen_w = 640;
  fh.screen_h = 360;
  fh.tile_w = 128;
  fh.tile_h = 128;

  std::vector<tile_blob> tiles(2);
  tiles[0].x = 0;
  tiles[0].y = 0;
  tiles[0].w = 128;
  tiles[0].h = 128;
  tiles[0].flags = kTileFlagChanged;
  tiles[0].data.assign(128 * 128 * 4, 0x7f);
  tiles[1].x = 128;
  tiles[1].y = 0;
  tiles[1].w = 64;
  tiles[1].h = 32;
  tiles[1].flags = kTileFlagChanged | kTileFlagKeyframe;
  tiles[1].data.assign(64 * 32 * 4, 0x11);

  std::vector<uint8_t> payload;
  std::string err;
  expect(build_frame_payload(fh, tiles, payload, err),
         "build_frame_payload succeeds: " + err);

  parsed_frame parsed;
  expect(parse_frame_payload(payload.data(), payload.size(), parsed, err),
         "parse_frame_payload succeeds: " + err);
  expect(parsed.header.frame_seq == 9, "frame_seq round-trips");
  expect(parsed.header.tile_count == 2, "tile_count round-trips");
  expect(parsed.header.codec == static_cast<uint8_t>(codec::bgra),
         "codec round-trips");
  expect(parsed.header.screen_w == 640 && parsed.header.screen_h == 360,
         "screen size round-trips");
  expect(parsed.tiles.size() == 2, "two tiles parse");
  if (parsed.tiles.size() == 2) {
    expect(parsed.tiles[0].x == 0 && parsed.tiles[0].y == 0,
           "tile 0 origin round-trips");
    expect(parsed.tiles[0].size == 128 * 128 * 4, "tile 0 size round-trips");
    expect(parsed.tiles[0].data == payload.data() + kFrameHeaderSize +
                                        2 * kTileEntrySize,
           "tile 0 points at the start of the tile area");
    expect(parsed.tiles[1].x == 128 && parsed.tiles[1].w == 64 &&
               parsed.tiles[1].h == 32,
           "tile 1 rect round-trips");
    expect(parsed.tiles[1].flags == (kTileFlagChanged | kTileFlagKeyframe),
           "tile 1 flags round-trip");
    expect(parsed.tiles[1].data == parsed.tiles[0].data + parsed.tiles[0].size,
           "tile 1 data follows tile 0");
    expect(parsed.tiles[1].data[0] == 0x11, "tile 1 bytes are intact");
  }
}

void test_frame_rejects() {
  using namespace molink::protocol;
  frame_header fh;
  fh.codec = static_cast<uint8_t>(codec::bgra);

  std::vector<tile_blob> tiles(1);
  tiles[0].w = 2;
  tiles[0].h = 2;
  tiles[0].data.assign(16, 0);

  std::vector<uint8_t> payload;
  std::string err;
  expect(build_frame_payload(fh, tiles, payload, err), "one-tile frame builds");

  {
    parsed_frame parsed;
    expect(!parse_frame_payload(payload.data(), payload.size() - 1, parsed, err),
           "truncated tile data is rejected");
  }
  {
    std::vector<uint8_t> bad = payload;
    // Tile byte_length claims more than the buffer holds.
    bad[kFrameHeaderSize + 12] = 0xff;
    bad[kFrameHeaderSize + 13] = 0xff;
    bad[kFrameHeaderSize + 14] = 0xff;
    bad[kFrameHeaderSize + 15] = 0x0f;
    parsed_frame parsed;
    expect(!parse_frame_payload(bad.data(), bad.size(), parsed, err),
           "tile overrun is rejected");
  }
  {
    std::vector<uint8_t> bad = payload;
    bad.push_back(0);
    parsed_frame parsed;
    expect(!parse_frame_payload(bad.data(), bad.size(), parsed, err),
           "trailing bytes are rejected");
  }
  {
    parsed_frame parsed;
    expect(!parse_frame_payload(payload.data(), 4, parsed, err),
           "short frame header is rejected");
  }
}

}  // namespace

int main() {
  test_packet_roundtrip();
  test_packet_rejects();
  test_frame_roundtrip();
  test_frame_rejects();

  if (g_failures == 0) {
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
  }
  std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
  return 1;
}
