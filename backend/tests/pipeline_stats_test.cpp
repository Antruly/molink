// The host-side media window: injected clock, fake frames and packets, and
// the exact counters the `video.pipeline` summary reports.
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "capture/pipeline_stats.h"

namespace {

int g_failures = 0;
int g_checks = 0;
uint64_t g_now = 0;

void expect(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

void test_trigger_names() {
  expect(std::string(molink::capture_trigger_name(
             molink::capture_trigger::timer)) == "timer",
         "the timer trigger has a stable name");
  expect(std::string(molink::capture_trigger_name(
             molink::capture_trigger::change)) == "change",
         "the change trigger has a stable name");
  expect(std::string(molink::capture_trigger_name(
             molink::capture_trigger::input)) == "input",
         "the input trigger has a stable name");
}

void test_percentile() {
  std::vector<uint64_t> samples;
  for (uint64_t i = 1; i <= 100; ++i) samples.push_back(i);
  expect(molink::percentile95(samples) == 95,
         "p95 of 1..100 is the 95th value");
  expect(molink::percentile95({}) == 0, "an empty sample set is zero");
  expect(molink::percentile95({7}) == 7, "one sample is its own p95");
  // Input order must not matter.
  expect(molink::percentile95({100, 1, 50, 2, 99}) == 100,
         "nearest-rank p95 of five samples is the maximum");
}

void test_window_and_counters() {
  g_now = 1000;
  molink::pipeline_monitor monitor([] { return g_now; }, 2000000);

  // 100 captures: trigger delay and encode time both 1..100 us.
  for (uint64_t i = 1; i <= 100; ++i) {
    monitor.note_capture(molink::capture_trigger::change, 1000, 1000 + i,
                         2000, 3000, 3000 + i, 0);
  }
  // One frame from a source that did not measure (all stamps zero): it still
  // counts as captured, but not as encoded and adds no samples.
  monitor.note_capture(molink::capture_trigger::timer, 0, 0, 0, 0, 0, 0);
  expect(!monitor.due(), "the window is not due before two seconds");

  // 100 packets, ages 1..100 ms. The monitor clocks the send at the injected
  // "now", so move the clock past the capture stamps first.
  g_now = 500000;
  for (uint64_t i = 1; i <= 100; ++i) {
    monitor.note_sent(500000 - i * 1000, 10);
  }
  monitor.note_sent_bytes(64);
  monitor.note_dropped(5);
  monitor.note_dropped(2);
  monitor.note_send_state(10, 40);
  monitor.note_send_state(35, 40);
  monitor.note_send_state(0, 10);

  g_now = 2001000;
  expect(monitor.due(), "the two-second boundary makes the window due");
  const molink::pipeline_snapshot snapshot = monitor.take();
  expect(snapshot.captured == 101, "every frame counts as captured");
  expect(snapshot.encoded == 100, "an untimed frame is not encoded");
  expect(snapshot.sent == 100, "every handed-over packet counts as sent");
  expect(snapshot.dropped == 7, "skipped frames accumulate");
  expect(snapshot.queue_depth == 30, "queue depth is the worst backlog seen");
  expect(snapshot.age_p95_us == 95000, "send age p95 is 95 ms");
  expect(snapshot.encode_p95_us == 95, "encode p95 is 95 us");
  expect(snapshot.trigger_delay_p95_us == 95, "trigger delay p95 is 95 us");
  expect(snapshot.input_delay_p95_us == 0, "no input sample stays zero");
  expect(snapshot.triggers[static_cast<std::size_t>(
             molink::capture_trigger::change)] == 100,
         "the trigger histogram counts the change frames");
  expect(snapshot.triggers[static_cast<std::size_t>(
             molink::capture_trigger::timer)] == 1,
         "the trigger histogram counts the timer frame");
  expect(snapshot.window_us == 2000000, "the window reports its length");
  expect(snapshot.kbps > 4.2 && snapshot.kbps < 4.3,
         "kbps is bytes over the window in kilobits");

  // take() starts a fresh window at the same instant.
  expect(!monitor.due(), "take rests the window");
  g_now += 10;
  const molink::pipeline_snapshot empty = monitor.take();
  expect(empty.captured == 0 && empty.sent == 0 && empty.dropped == 0,
         "take resets the counters");
  expect(empty.queue_depth == 0, "take resets the queue depth");
}

void test_input_delay() {
  g_now = 500;
  molink::pipeline_monitor monitor([] { return g_now; }, 2000000);
  monitor.note_capture(molink::capture_trigger::change, 500, 1000, 2000, 2000,
                       3000, 30000);
  monitor.note_capture(molink::capture_trigger::change, 500, 1000, 2000, 2000,
                       3000, 100000);
  monitor.note_capture(molink::capture_trigger::change, 500, 1000, 2000, 2000,
                       3000, 0);
  g_now += 2000000;
  const molink::pipeline_snapshot snapshot = monitor.take();
  expect(snapshot.input_delay_p95_us == 100000,
         "input-to-grab delay uses only recent-input samples");
}

void test_peek_does_not_reset() {
  g_now = 100;
  molink::pipeline_monitor monitor([] { return g_now; }, 1000);
  monitor.note_capture(molink::capture_trigger::timer, 100, 200, 300, 300,
                       400, 0);
  const molink::pipeline_snapshot first = monitor.peek();
  const molink::pipeline_snapshot second = monitor.peek();
  expect(first.captured == 1 && second.captured == 1,
         "peek leaves the window intact");
  g_now += 1000;
  expect(monitor.take().captured == 1, "take returns the peeked window");
  expect(monitor.peek().captured == 0, "the next window is empty");
}

void test_sample_cap_keeps_the_distribution() {
  g_now = 0;
  molink::pipeline_monitor monitor([] { return g_now; }, 2000000);
  // More samples than the cap: every value 1..N appears, so the p95 must be
  // near the top of the range rather than pinned to an early truncation.
  const uint64_t count = molink::pipeline_monitor::kMaxSamples * 2 + 500;
  for (uint64_t i = 1; i <= count; ++i) {
    monitor.note_capture(molink::capture_trigger::change, 1000, 1000, 2000,
                         2000, 2000 + i, 0);
  }
  g_now += 2000000;
  const molink::pipeline_snapshot snapshot = monitor.take();
  expect(snapshot.encoded == count, "every frame is still counted");
  expect(snapshot.encode_p95_us > count / 2,
         "the capped sample set still reflects the upper tail");
}

}  // namespace

int main() {
  test_trigger_names();
  test_percentile();
  test_window_and_counters();
  test_input_delay();
  test_peek_does_not_reset();
  test_sample_cap_keeps_the_distribution();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
