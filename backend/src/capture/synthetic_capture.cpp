#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "capture/capture.h"
#include "protocol/frame_packet.h"
#include "test_pattern.h"

namespace molink {
namespace {

uint64_t epoch_us() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(system_clock::now().time_since_epoch())
          .count());
}

/// Fallback source: the animated test pattern, exposed through the same
/// interface as a real display so the rest of the pipeline never branches on
/// where frames came from.
class synthetic_capture : public capture_base {
 public:
  explicit synthetic_capture(const capture_options& options) : capture_base(options.fps), options_(options) {}
  ~synthetic_capture() override { stop(); }

  bool start(std::string& err) override {
    if (running_.load()) return true;

    pattern_.width = options_.synthetic_width;
    pattern_.height = options_.synthetic_height;
    pattern_.tile = 128;

    // Validate the geometry once so start() can report a real error.
    std::vector<uint8_t> probe;
    bool keyframe = false;
    if (!build_test_frame_payload(0, pattern_, probe, keyframe, err)) {
      return false;
    }

    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void stop() override {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
  }

  std::string backend() const override { return "synthetic"; }
  bool is_real() const override { return false; }

  std::vector<display_info> displays() const override {
    return {current_display_info()};
  }
  int current_display() const override { return 0; }
  display_info current_display_info() const override {
    display_info info;
    info.name = "Synthetic";
    info.width = pattern_.width;
    info.height = pattern_.height;
    info.input_width = pattern_.width;
    info.input_height = pattern_.height;
    info.primary = true;
    return info;
  }
  bool select_display(int index) override { return index == 0; }

 private:
  void run() {
    auto next = std::chrono::steady_clock::now();
    uint32_t seq = 0;

    // One frame up front so the welcome message and /api/info can report the
    // screen size before any client asks for pixels.
    publish_one(seq++);

    while (running_.load()) {
      if (!active_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        next = std::chrono::steady_clock::now();
        continue;
      }

      std::vector<uint8_t> payload;
      bool keyframe = false;
      std::string err;
      const uint64_t capture_start_us = epoch_us();
      if (build_test_frame_payload(seq, pattern_, payload, keyframe, err)) {
        const uint64_t capture_done_us = epoch_us();
        auto frame = std::make_shared<captured_frame>();
        frame->seq = seq++;
        frame->ts_us = capture_done_us;
        frame->capture_start_us = capture_start_us;
        frame->capture_done_us = capture_done_us;
        // The synthetic source has no separate grab and encode: the pattern
        // build is both. Report it as the encode so the latency line has a
        // number for a host running on the test source.
        frame->encode_start_us = capture_start_us;
        frame->encode_end_us = capture_done_us;
        frame->trigger = capture_trigger::timer;
        frame->trigger_us = capture_start_us;
        frame->keyframe = keyframe;
        frame->width = pattern_.width;
        frame->height = pattern_.height;
        frame->tile_w = pattern_.tile;
        frame->tile_h = pattern_.tile;
        frame->codec = static_cast<uint8_t>(protocol::codec::bgra);
        frame->payload = std::move(payload);
        publish(std::move(frame));
      }

      next += std::chrono::milliseconds(1000 / std::max(1, target_fps()));
      const auto now = std::chrono::steady_clock::now();
      if (next < now) next = now;
      std::this_thread::sleep_until(next);
    }
  }

  void publish_one(uint32_t seq) {
    std::vector<uint8_t> payload;
    bool keyframe = false;
    std::string err;
    const uint64_t capture_start_us = epoch_us();
    if (!build_test_frame_payload(seq, pattern_, payload, keyframe, err)) return;
    const uint64_t capture_done_us = epoch_us();
    auto frame = std::make_shared<captured_frame>();
    frame->seq = seq;
    frame->ts_us = capture_done_us;
    frame->capture_start_us = capture_start_us;
    frame->capture_done_us = capture_done_us;
    frame->encode_start_us = capture_start_us;
    frame->encode_end_us = capture_done_us;
    frame->trigger = capture_trigger::initial;
    frame->trigger_us = capture_start_us;
    frame->keyframe = keyframe;
    frame->width = pattern_.width;
    frame->height = pattern_.height;
    frame->tile_w = pattern_.tile;
    frame->tile_h = pattern_.tile;
    frame->codec = static_cast<uint8_t>(protocol::codec::bgra);
    frame->payload = std::move(payload);
    publish(std::move(frame));
  }

  capture_options options_;
  test_pattern_options pattern_;
  std::thread thread_;
};

}  // namespace

std::unique_ptr<capture_source> make_synthetic_capture(
    const capture_options& options) {
  return std::unique_ptr<capture_source>(new synthetic_capture(options));
}

}  // namespace molink
