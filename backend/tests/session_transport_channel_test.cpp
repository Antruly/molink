#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "transport/session_transport.h"

namespace {

int checks = 0;
int failures = 0;

void expect(bool value, const char* message) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

class legacy_transport final : public molink::session_transport {
 public:
  const char* name() const override { return "legacy"; }
  std::string peer_ip() const override { return "test"; }
  unsigned int peer_port() const override { return 0; }
  bool is_open() const override { return true; }
  bool send_text(const std::string& text) override {
    texts.push_back(text);
    return true;
  }
  bool send_binary(const uint8_t* data, std::size_t len) override {
    binaries.emplace_back(data, data + len);
    return true;
  }
  void close(int, const std::string&) override {}
  void set_handlers(text_handler, binary_handler, close_handler) override {}

  std::vector<std::string> texts;
  std::vector<std::vector<uint8_t>> binaries;
};

class channel_transport final : public molink::session_transport {
 public:
  const char* name() const override { return "channels"; }
  std::string peer_ip() const override { return "test"; }
  unsigned int peer_port() const override { return 0; }
  bool is_open() const override { return true; }
  bool send_text(const std::string&) override {
    ++legacy_text_calls;
    return true;
  }
  bool send_binary(const uint8_t*, std::size_t) override {
    ++legacy_binary_calls;
    return true;
  }
  bool send_text(molink::transport_channel channel,
                 const std::string& text) override {
    text_channels.push_back(channel);
    texts.push_back(text);
    return true;
  }
  bool send_binary(molink::transport_channel channel, const uint8_t* data,
                   std::size_t len) override {
    binary_channels.push_back(channel);
    binaries.emplace_back(data, data + len);
    return true;
  }
  void close(int, const std::string&) override {}
  void set_handlers(text_handler, binary_handler, close_handler) override {}

  int legacy_text_calls = 0;
  int legacy_binary_calls = 0;
  std::vector<molink::transport_channel> text_channels;
  std::vector<molink::transport_channel> binary_channels;
  std::vector<std::string> texts;
  std::vector<std::vector<uint8_t>> binaries;
};

void test_legacy_fallback() {
  legacy_transport transport;
  const uint8_t byte = 0x42;
  molink::session_transport& base = transport;
  expect(base.send_text(molink::transport_channel::control, "control"),
         "legacy control text succeeds");
  expect(base.send_binary(molink::transport_channel::video, &byte, 1),
         "legacy video binary succeeds");
  expect(transport.texts.size() == 1 && transport.texts[0] == "control",
         "legacy text uses existing API");
  expect(transport.binaries.size() == 1 && transport.binaries[0][0] == byte,
         "legacy binary uses existing API");
}

void test_channel_override() {
  channel_transport transport;
  const uint8_t byte = 0x24;
  transport.send_text(molink::transport_channel::control, "hello");
  transport.send_binary(molink::transport_channel::video, &byte, 1);
  transport.send_binary(molink::transport_channel::audio, &byte, 1);
  transport.send_binary(molink::transport_channel::file, &byte, 1);
  expect(transport.text_channels.size() == 1 &&
             transport.text_channels[0] == molink::transport_channel::control,
         "specialized transport receives control channel");
  expect(transport.binary_channels.size() == 3 &&
             transport.binary_channels[0] == molink::transport_channel::video &&
             transport.binary_channels[1] == molink::transport_channel::audio &&
             transport.binary_channels[2] == molink::transport_channel::file,
         "specialized transport keeps binary channels separate");
  expect(transport.legacy_text_calls == 0 && transport.legacy_binary_calls == 0,
         "specialized transport bypasses legacy methods");
}

}  // namespace

int main() {
  test_legacy_fallback();
  test_channel_override();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
