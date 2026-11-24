// Bounded TCP backlog: video is droppable, reliable channels are not.
#include <cstddef>
#include <cstdio>

#include "transport/tcp_backlog_policy.h"

namespace {

int checks = 0;
int failures = 0;

void expect(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

using molink::tcp_backlog_action;
using molink::tcp_backlog_admit;
using molink::transport_channel;

constexpr std::size_t kLimit = 8u * 1024u * 1024u;

void test_room_is_accepted() {
  expect(tcp_backlog_admit(transport_channel::video, 0, 1024, kLimit) ==
             tcp_backlog_action::accept,
         "a video frame with room is queued");
  expect(tcp_backlog_admit(transport_channel::control, kLimit - 10, 10,
                           kLimit) == tcp_backlog_action::accept,
         "a message that exactly fills the queue is accepted");
}

void test_video_is_dropped_not_disconnected() {
  expect(tcp_backlog_admit(transport_channel::video, kLimit - 10, 100,
                           kLimit) == tcp_backlog_action::drop_video,
         "video past the backlog is dropped, not a link failure");
  // Even a video frame bigger than the whole budget is a drop: the session's
  // reliable channels can still make progress.
  expect(tcp_backlog_admit(transport_channel::video, 0, kLimit * 2, kLimit) ==
             tcp_backlog_action::drop_video,
         "an oversized video frame does not fail the link");
}

void test_reliable_channels_still_fail() {
  for (const transport_channel channel :
       {transport_channel::control, transport_channel::audio,
        transport_channel::file}) {
    expect(tcp_backlog_admit(channel, kLimit - 10, 100, kLimit) ==
               tcp_backlog_action::fail_link,
           "a reliable message past the backlog is a genuine failure");
  }
}

}  // namespace

int main() {
  test_room_is_accepted();
  test_video_is_dropped_not_disconnected();
  test_reliable_channels_still_fail();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
