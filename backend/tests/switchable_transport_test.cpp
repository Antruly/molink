// Checks the relay/ICE split. The sender moves keyframes, AV1 patches, tile
// refreshes, tile clocks, file data and repair requests to the relay once ICE
// is active; the receiving switchable must accept exactly the same set from
// the relay and leave everything else on ICE. Regression test for the split
// that silently dropped every repair packet after a P2P switch.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "protocol/frame_packet.h"
#include "transport/switchable_transport.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void expect(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("FAIL: %s\n", what);
  }
}

using namespace molink;

class fake_transport final : public session_transport {
 public:
  explicit fake_transport(std::string label) : label_(std::move(label)) {}

  const char* name() const override { return label_.c_str(); }
  std::string peer_ip() const override { return label_; }
  unsigned int peer_port() const override { return 0; }
  bool is_open() const override { return open_; }
  bool send_text(const std::string& text) override {
    sent_text.push_back(text);
    return open_;
  }
  bool send_binary(const uint8_t* data, std::size_t len) override {
    sent_binary.emplace_back(data, data + len);
    return open_;
  }
  void close(int, const std::string&) override { open_ = false; }
  void set_handlers(text_handler on_text, binary_handler on_binary,
                    close_handler on_close) override {
    text_ = std::move(on_text);
    binary_ = std::move(on_binary);
    close_ = std::move(on_close);
  }

  void receive_text(const std::string& text) {
    if (text_) text_(text);
  }
  void receive_binary(const std::vector<uint8_t>& bytes) {
    if (binary_) binary_(bytes.data(), bytes.size());
  }

  std::vector<std::string> sent_text;
  std::vector<std::vector<uint8_t>> sent_binary;

 private:
  std::string label_;
  bool open_ = true;
  text_handler text_;
  binary_handler binary_;
  close_handler close_;
};

std::vector<uint8_t> packet(uint8_t type, uint16_t flags,
                            const std::string& payload) {
  std::vector<uint8_t> bytes;
  std::string err;
  protocol::encode_packet_into(
      bytes, type, flags, 1, 1, 1000,
      reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), err);
  return bytes;
}

std::string frame_payload(uint8_t codec, uint8_t frame_flags,
                          bool with_tile) {
  protocol::frame_header header;
  header.frame_seq = 7;
  header.codec = codec;
  header.flags = frame_flags;
  header.screen_w = 256;
  header.screen_h = 128;
  header.tile_w = 128;
  header.tile_h = 128;
  std::vector<protocol::tile_blob> tiles;
  if (with_tile) {
    protocol::tile_blob tile;
    tile.x = 0;
    tile.y = 0;
    tile.w = 128;
    tile.h = 128;
    tile.flags = protocol::kTileFlagChanged;
    tile.data.assign(128 * 128 * 4, 0x40);
    tiles.push_back(std::move(tile));
  }
  std::vector<uint8_t> out;
  std::string err;
  protocol::build_frame_payload(header, tiles, out, err);
  return std::string(out.begin(), out.end());
}

std::vector<uint8_t> keyframe_packet() {
  return packet(static_cast<uint8_t>(protocol::packet_type::frame), 0,
                frame_payload(static_cast<uint8_t>(protocol::codec::bgra),
                              protocol::kFrameFlagKeyframe, true));
}

std::vector<uint8_t> delta_packet() {
  return packet(static_cast<uint8_t>(protocol::packet_type::frame), 0,
                frame_payload(static_cast<uint8_t>(protocol::codec::bgra),
                              protocol::kFrameFlagPartial, true));
}

std::vector<uint8_t> av1_packet() {
  return packet(static_cast<uint8_t>(protocol::packet_type::frame), 0,
                frame_payload(static_cast<uint8_t>(protocol::codec::av1),
                              protocol::kFrameFlagPartial, true));
}

std::vector<uint8_t> tile_refresh_packet() {
  return packet(static_cast<uint8_t>(protocol::packet_type::frame),
                protocol::kPacketFlagTileRefresh,
                frame_payload(static_cast<uint8_t>(protocol::codec::bgra),
                              protocol::kFrameFlagPartial, true));
}

std::vector<uint8_t> tile_clock_packet() {
  return packet(static_cast<uint8_t>(protocol::packet_type::tile_clock), 0,
                std::string(8 + 8, '\0'));
}

std::vector<uint8_t> file_data_packet() {
  return packet(static_cast<uint8_t>(protocol::packet_type::file_data), 0,
                std::string(16, 'x'));
}

void test_relay_split() {
  auto relay = std::make_shared<fake_transport>("relay");
  auto ice = std::make_shared<fake_transport>("ice");
  auto switcher = std::make_shared<switchable_transport>(relay);

  std::vector<std::string> texts;
  std::vector<std::vector<uint8_t>> binaries;
  switcher->set_handlers(
      [&texts](const std::string& text) { texts.push_back(text); },
      [&binaries](const uint8_t* data, std::size_t len) {
        binaries.emplace_back(data, data + len);
      },
      [](int, const std::string&) {});

  // Before the switch every packet arrives on the relay.
  relay->receive_text("{\"type\":\"ping\"}");
  relay->receive_binary(delta_packet());
  expect(texts.size() == 1 && binaries.size() == 1,
         "relay is fully read before the switch");

  texts.clear();
  binaries.clear();
  switcher->set_ice(ice);
  expect(switcher->switched(), "transport switches to ICE");

  // Reliable traffic keeps using the relay after the switch.
  relay->receive_text("{\"type\":\"request-tiles\",\"tiles\":[]}");
  relay->receive_text("{\"type\":\"request-keyframe\"}");
  relay->receive_binary(keyframe_packet());
  relay->receive_binary(av1_packet());
  relay->receive_binary(tile_refresh_packet());
  relay->receive_binary(tile_clock_packet());
  relay->receive_binary(file_data_packet());
  expect(texts.size() == 2, "repair requests are accepted from the relay");
  expect(binaries.size() == 5,
         "keyframe/AV1/refresh/clock/file data are accepted from the relay");

  // Ordinary control messages and deltas stay on ICE.
  texts.clear();
  binaries.clear();
  relay->receive_text("{\"type\":\"ping\"}");
  relay->receive_binary(delta_packet());
  expect(texts.empty() && binaries.empty(),
         "relay drops deltas and ordinary text after the switch");

  ice->receive_text("{\"type\":\"ping\"}");
  ice->receive_binary(delta_packet());
  expect(texts.size() == 1 && binaries.size() == 1,
         "ICE carries deltas and ordinary text");

  // Outbound routing matches the inbound split.
  relay->sent_text.clear();
  relay->sent_binary.clear();
  ice->sent_text.clear();
  ice->sent_binary.clear();
  switcher->send_text("{\"type\":\"request-tiles\",\"tiles\":[]}");
  switcher->send_text("{\"type\":\"ping\"}");
  const auto delta = delta_packet();
  switcher->send_binary(delta.data(), delta.size());
  const auto keyframe = keyframe_packet();
  switcher->send_binary(keyframe.data(), keyframe.size());
  expect(relay->sent_text.size() == 1 && ice->sent_text.size() == 1,
         "request-tiles goes to the relay and ping to ICE");
  expect(relay->sent_binary.size() == 1 && ice->sent_binary.size() == 1,
         "keyframe goes to the relay and delta to ICE");
}

}  // namespace

int main() {
  test_relay_split();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
