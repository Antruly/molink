// Regression test for the transport lifetime crash (issue #146).
//
// The pushed-frame clock used to hand a raw `session_transport*` to
// send_frame_payload(). libuvcpp recycles a WebSocket connection on a later
// loop turn, so a frame that lost the race against teardown compressed through
// a freed zlib stream and crashed inside deflate.
//
// send_frame_packet() is the transport-facing half of that path. It must lock
// the session's weak transport reference *before* it advances the wire
// sequence or builds any bytes, and it must do nothing once the transport has
// been closed or destroyed. This test drives it with a fake transport and no
// network or event loop.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "transport/video_send.h"

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

class fake_transport final : public molink::session_transport {
 public:
  const char* name() const override { return "fake"; }
  std::string peer_ip() const override { return "127.0.0.1"; }
  unsigned int peer_port() const override { return 1; }

  bool is_open() const override { return open_; }
  bool send_text(const std::string&) override {
    ++texts;
    return open_;
  }
  bool send_binary(const std::uint8_t*, std::size_t) override {
    ++binaries;
    return open_ && !refuse_;
  }
  void close(int, const std::string&) override {
    open_ = false;
    ++closes;
  }
  void set_handlers(text_handler, binary_handler, close_handler) override {}

  bool open_ = true;
  bool refuse_ = false;
  int binaries = 0;
  int texts = 0;
  int closes = 0;
};

std::vector<std::uint8_t> sample_payload() {
  // One byte is enough: encode_packet_into() only wraps it in the MOLK header.
  return {0x42};
}

void test_live_transport_sends() {
  auto transport = std::make_shared<fake_transport>();
  auto session = std::make_shared<molink::session_info>();
  session->id = 7;
  session->transport = transport;

  for (int i = 0; i < 100; ++i) {
    const molink::video_packet_result result = molink::send_frame_packet(
        session, 0, 1000 + static_cast<std::uint64_t>(i), sample_payload());
    expect(result.status == molink::video_packet_status::sent,
           "a live transport accepts every frame");
  }
  expect(transport->binaries == 100, "exactly 100 packets reached the wire");
  expect(session->frames_sent == 100, "frames_sent counts accepted frames");
  expect(session->out_seq == 100, "the wire sequence advanced once per frame");
}

void test_destroyed_transport_skips_frames() {
  auto transport = std::make_shared<fake_transport>();
  auto session = std::make_shared<molink::session_info>();
  session->id = 8;
  session->transport = transport;

  const molink::video_packet_result first = molink::send_frame_packet(
      session, 0, 1, sample_payload());
  expect(first.status == molink::video_packet_status::sent,
         "the first frame is sent before teardown");
  const std::uint32_t seq_after_first = session->out_seq;

  // This is the crash: the connection is gone (the controller released its
  // owning reference) while the session record is still reachable.
  session->transport.reset();
  transport.reset();

  for (int i = 0; i < 100; ++i) {
    const molink::video_packet_result result = molink::send_frame_packet(
        session, 0, 2 + static_cast<std::uint64_t>(i), sample_payload());
    expect(result.status == molink::video_packet_status::no_transport,
           "a frame for a destroyed transport is skipped");
  }
  expect(session->out_seq == seq_after_first,
         "a skipped frame does not advance the wire sequence");
  expect(session->frames_sent == 1, "a skipped frame is not counted as sent");
}

void test_closed_transport_skips_frames() {
  auto transport = std::make_shared<fake_transport>();
  auto session = std::make_shared<molink::session_info>();
  session->id = 9;
  session->transport = transport;

  transport->close(1000, "test");
  expect(transport->closes == 1, "close() reached the fake transport");

  for (int i = 0; i < 100; ++i) {
    const molink::video_packet_result result = molink::send_frame_packet(
        session, 0, static_cast<std::uint64_t>(i), sample_payload());
    expect(result.status == molink::video_packet_status::refused_closed,
           "a closed transport refuses without reporting backpressure");
  }
  expect(session->frames_sent == 0, "nothing is counted on a closed transport");
}

void test_backpressure_is_reported() {
  auto transport = std::make_shared<fake_transport>();
  auto session = std::make_shared<molink::session_info>();
  session->id = 10;
  session->transport = transport;
  transport->refuse_ = true;

  const molink::video_packet_result result = molink::send_frame_packet(
      session, 0, 1, sample_payload());
  expect(result.status == molink::video_packet_status::refused_open,
         "a live but saturated transport is backpressure, not a dead link");
}

}  // namespace

int main() {
  test_live_transport_sends();
  test_destroyed_transport_skips_frames();
  test_closed_transport_skips_frames();
  test_backpressure_is_reported();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d/%d checks failed\n", g_failures, g_checks);
    return 1;
  }
  std::printf("video_send: %d checks passed\n", g_checks);
  return 0;
}
