#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "transport/channel_scheduler.h"

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

std::vector<uint8_t> bytes(std::size_t size, uint8_t value) {
  return std::vector<uint8_t>(size, value);
}

void test_reliable_budgets() {
  molink::channel_scheduler::limits limits;
  limits.control_bytes = 4;
  limits.audio_bytes = 4;
  limits.file_bytes = 4;
  limits.video_bytes = 8;
  molink::channel_scheduler scheduler(limits);
  const auto data = bytes(3, 0x11);

  expect(scheduler.enqueue(molink::transport_channel::control, data.data(),
                           data.size()),
         "control message fits its budget");
  expect(!scheduler.enqueue(molink::transport_channel::control, data.data(),
                            data.size()),
         "control applies backpressure at its budget");
  expect(scheduler.queued_bytes(molink::transport_channel::control) == 3,
         "control budget is independent");
  expect(scheduler.enqueue(molink::transport_channel::file, data.data(),
                           data.size()),
         "file has an independent budget");
}

void test_video_keeps_latest_keyframe_and_deltas() {
  molink::channel_scheduler::limits limits;
  limits.video_bytes = 16;
  molink::channel_scheduler scheduler(limits);
  const auto old_delta = bytes(4, 0x21);
  const auto old_keyframe = bytes(4, 0x28);
  const auto stale_delta = bytes(4, 0x29);
  const auto keyframe = bytes(4, 0x30);
  const auto new_delta = bytes(4, 0x31);

  expect(scheduler.enqueue(molink::transport_channel::video, old_delta.data(),
                           old_delta.size()),
         "first video delta queues");
  expect(scheduler.enqueue(molink::transport_channel::video, old_keyframe.data(),
                           old_keyframe.size(), true),
         "old keyframe queues");
  expect(scheduler.enqueue(molink::transport_channel::video, stale_delta.data(),
                           stale_delta.size()),
         "delta after old keyframe queues");
  expect(scheduler.enqueue(molink::transport_channel::video, keyframe.data(),
                           keyframe.size(), true),
         "new keyframe supersedes all previously queued video");
  expect(scheduler.enqueue(molink::transport_channel::video, new_delta.data(),
                           new_delta.size()),
         "delta after latest keyframe queues");
  expect(scheduler.queued_messages(molink::transport_channel::video) == 2,
         "queue contains only newest keyframe and following delta");

  std::vector<std::vector<uint8_t>> sent_video;
  scheduler.drain(
      [&sent_video](const molink::channel_scheduler::message& message) {
        if (message.channel == molink::transport_channel::video) {
          sent_video.push_back(message.data);
        }
        return true;
      });
  expect(sent_video.size() == 2 && sent_video[0] == keyframe &&
             sent_video[1] == new_delta,
         "drain never sends stale deltas ahead of the newest keyframe");
}

void test_drain_priority_and_backpressure() {
  molink::channel_scheduler scheduler;
  const auto data = bytes(1, 0x41);
  scheduler.enqueue(molink::transport_channel::file, data.data(), data.size());
  scheduler.enqueue(molink::transport_channel::video, data.data(), data.size());
  scheduler.enqueue(molink::transport_channel::audio, data.data(), data.size());
  scheduler.enqueue(molink::transport_channel::control, data.data(), data.size());

  std::vector<molink::transport_channel> order;
  const std::size_t sent = scheduler.drain(
      [&order](const molink::channel_scheduler::message& message) {
        order.push_back(message.channel);
        return true;
      });
  expect(sent == 4 && order.size() == 4 &&
             order[0] == molink::transport_channel::control &&
             order[1] == molink::transport_channel::audio &&
             order[2] == molink::transport_channel::video &&
             order[3] == molink::transport_channel::file,
         "drain sends channels in control, audio, video, file priority");
}

void test_drain_keeps_all_backpressured_messages() {
  molink::channel_scheduler scheduler;
  const auto data = bytes(1, 0x41);
  scheduler.enqueue(molink::transport_channel::file, data.data(), data.size());
  scheduler.enqueue(molink::transport_channel::control, data.data(), data.size());
  scheduler.enqueue(molink::transport_channel::audio, data.data(), data.size());

  std::vector<molink::transport_channel> order;
  const std::size_t sent = scheduler.drain(
      [&order](const molink::channel_scheduler::message& message) {
        order.push_back(message.channel);
        return false;
      });
  expect(sent == 0 && order.size() == 3 &&
             order[0] == molink::transport_channel::control &&
             order[1] == molink::transport_channel::audio &&
             order[2] == molink::transport_channel::file,
         "failed sends keep all messages queued without dropping them");
  expect(scheduler.queued_messages(molink::transport_channel::control) == 1 &&
             scheduler.queued_messages(molink::transport_channel::audio) == 1 &&
             scheduler.queued_messages(molink::transport_channel::file) == 1,
         "all backpressured messages remain queued");

  order.clear();
  const std::size_t retried = scheduler.drain(
      [&order](const molink::channel_scheduler::message& message) {
        order.push_back(message.channel);
        return true;
      });
  expect(retried == 3 && scheduler.empty() && order.size() == 3,
         "a later drain retries and clears previously blocked channels");
}

void test_sustained_file_backpressure_preserves_backlog_and_control() {
  molink::channel_scheduler::limits limits;
  limits.control_bytes = 4 * 1024;
  limits.file_bytes = 64 * 1024;
  molink::channel_scheduler scheduler(limits);
  const auto file_data = bytes(257, 0x5a);
  const auto control_data = bytes(3, 0xc1);

  std::size_t file_messages = 0;
  std::vector<uint8_t> file_sequence;
  while (true) {
    auto file_data = bytes(257, static_cast<uint8_t>(file_messages));
    if (!scheduler.enqueue(molink::transport_channel::file, file_data.data(),
                           file_data.size())) {
      break;
    }
    file_sequence.push_back(file_data.front());
    ++file_messages;
  }
  expect(file_messages > 200,
         "stress case fills a substantial bounded file backlog");
  const std::size_t file_bytes = scheduler.queued_bytes(
      molink::transport_channel::file);
  expect(file_bytes <= limits.file_bytes &&
             scheduler.queued_messages(molink::transport_channel::file) ==
                 file_messages,
         "file backlog stays within its configured byte budget");

  constexpr std::size_t control_messages = 32;
  for (std::size_t index = 0; index < control_messages; ++index) {
    expect(scheduler.enqueue(molink::transport_channel::control,
                             control_data.data(), control_data.size()),
           "control messages continue to fit while file is backpressured");
  }

  std::vector<molink::transport_channel> sent;
  const std::size_t control_sent = scheduler.drain(
      [&sent](const molink::channel_scheduler::message& message) {
        sent.push_back(message.channel);
        return message.channel != molink::transport_channel::file;
      });
  expect(control_sent == control_messages && sent.size() == control_messages + 1,
         "control messages drain despite sustained file backpressure");
  expect(std::find(sent.begin(), sent.end(),
                   molink::transport_channel::file) != sent.end(),
         "file backpressure is attempted after control messages");
  expect(scheduler.queued_bytes(molink::transport_channel::file) == file_bytes &&
             scheduler.queued_messages(molink::transport_channel::file) ==
                 file_messages,
         "failed file sends retain the complete backlog");

  std::size_t released_file_messages = 0;
  std::vector<uint8_t> released_sequence;
  const std::size_t released = scheduler.drain(
      [&released_file_messages, &released_sequence](
          const molink::channel_scheduler::message& message) {
        if (message.channel == molink::transport_channel::file) {
          ++released_file_messages;
          released_sequence.push_back(message.data.front());
        }
        return true;
      });
  expect(released == file_messages &&
             released_file_messages == file_messages &&
             released_sequence == file_sequence && scheduler.empty(),
         "releasing file backpressure drains every queued file message in order");
}

void test_drain_isolates_channel_backpressure() {
  molink::channel_scheduler scheduler;
  const auto data = bytes(1, 0x41);
  scheduler.enqueue(molink::transport_channel::file, data.data(), data.size());
  scheduler.enqueue(molink::transport_channel::control, data.data(), data.size());
  scheduler.enqueue(molink::transport_channel::audio, data.data(), data.size());
  scheduler.enqueue(molink::transport_channel::video, data.data(), data.size());

  std::vector<molink::transport_channel> attempts;
  const std::size_t sent = scheduler.drain(
      [&attempts](const molink::channel_scheduler::message& message) {
        attempts.push_back(message.channel);
        return message.channel != molink::transport_channel::control;
      });
  expect(sent == 3 && attempts.size() == 4 &&
             attempts[0] == molink::transport_channel::control &&
             attempts[1] == molink::transport_channel::audio &&
             attempts[2] == molink::transport_channel::video &&
             attempts[3] == molink::transport_channel::file,
         "backpressure blocks only its channel while others drain");
  expect(scheduler.queued_messages(molink::transport_channel::control) == 1 &&
             scheduler.queued_messages(molink::transport_channel::audio) == 0 &&
             scheduler.queued_messages(molink::transport_channel::video) == 0 &&
             scheduler.queued_messages(molink::transport_channel::file) == 0,
         "failed channel retains its message and independent queues drain");
}

void test_single_video_frame_larger_than_budget() {
  molink::channel_scheduler::limits limits;
  limits.video_bytes = 1024;
  molink::channel_scheduler scheduler(limits);
  // A lossless full-screen keyframe is tens of MiB; it can never fit a byte
  // budget, but refusing it means the picture never arrives at all.
  const auto big_keyframe = bytes(4096, 0x77);
  expect(scheduler.enqueue(molink::transport_channel::video,
                           big_keyframe.data(), big_keyframe.size(), true),
         "a keyframe larger than the video budget still queues");
  const auto delta = bytes(512, 0x78);
  expect(!scheduler.enqueue(molink::transport_channel::video, delta.data(),
                            delta.size()),
         "a delta is dropped while the oversized keyframe is pending");

  std::vector<std::vector<uint8_t>> sent;
  scheduler.drain([&sent](const molink::channel_scheduler::message& message) {
    sent.push_back(message.data);
    return true;
  });
  expect(sent.size() == 1 && sent.front() == big_keyframe,
         "the oversized keyframe drains intact and alone");

  // Once it is gone the deltas flow again.
  expect(scheduler.enqueue(molink::transport_channel::video, delta.data(),
                           delta.size()),
         "small frames queue again after the oversized frame drains");
}

void test_control_holds_a_large_clipboard_message() {
  // The default control budget has to clear the 192 KiB clipboard-sync cap
  // plus JSON framing, and a second one on top must apply backpressure rather
  // than corrupt the first.
  molink::channel_scheduler scheduler;
  const auto clipboard = bytes(192 * 1024 + 1024, 0x63);
  expect(scheduler.enqueue(molink::transport_channel::control,
                           clipboard.data(), clipboard.size()),
         "a 193 KiB clipboard message fits the control budget");
  expect(!scheduler.enqueue(molink::transport_channel::control,
                            clipboard.data(), clipboard.size()),
         "the next clipboard message is refused, not silently dropped");
  expect(scheduler.queued_bytes(molink::transport_channel::control) ==
             clipboard.size(),
         "the queue keeps exactly the message it accepted");
}

}  // namespace

int main() {
  test_reliable_budgets();
  test_video_keeps_latest_keyframe_and_deltas();
  test_drain_priority_and_backpressure();
  test_drain_keeps_all_backpressured_messages();
  test_sustained_file_backpressure_preserves_backlog_and_control();
  test_drain_isolates_channel_backpressure();
  test_single_video_frame_larger_than_budget();
  test_control_holds_a_large_clipboard_message();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
