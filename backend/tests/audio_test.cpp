// Audio hub: the silence gate, the integer downmix, and the one shared PCM
// buffer per packet. The source is fake and the clock is injected, so the test
// neither captures nor sleeps.

#include "audio/audio.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                   #cond);                                             \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

using molink::audio_format;
using molink::audio_hub;
using molink::audio_hub_options;
using molink::audio_hub_stats;
using molink::audio_pcm_ptr;
using molink::audio_source;

class fake_source final : public audio_source {
 public:
  explicit fake_source(audio_format format) : format_(format) {}

  bool available() const override { return true; }
  audio_format format() const override { return format_; }
  bool start(sink callback, std::string&) override {
    sink_ = std::move(callback);
    return true;
  }
  void stop() override { sink_ = nullptr; }

  void emit_samples(const std::vector<int16_t>& samples) {
    std::vector<uint8_t> bytes(samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const uint16_t value = static_cast<uint16_t>(samples[i]);
      bytes[i * 2] = static_cast<uint8_t>(value & 0xffu);
      bytes[i * 2 + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
    }
    emit_bytes(std::move(bytes));
  }

  void emit_bytes(std::vector<uint8_t> bytes) {
    if (sink_) sink_(std::move(bytes));
  }

 private:
  audio_format format_;
  sink sink_;
};

std::vector<int16_t> constant_pcm(int frames, int channels, int16_t value) {
  return std::vector<int16_t>(
      static_cast<std::size_t>(frames) * channels, value);
}

std::vector<int16_t> stereo_pcm(int frames, int16_t left, int16_t right) {
  std::vector<int16_t> samples(static_cast<std::size_t>(frames) * 2);
  for (int frame = 0; frame < frames; ++frame) {
    samples[frame * 2] = left;
    samples[frame * 2 + 1] = right;
  }
  return samples;
}

void test_silence_gate() {
  audio_hub_options options;
  options.quality = "high";
  options.silence_packets = 3;
  uint64_t clock = 1000;
  options.now_ms = [&clock] { return clock; };
  std::vector<std::pair<std::string, uint64_t>> logs;
  options.gate_log = [&logs](const char* state, uint64_t silent_ms) {
    logs.emplace_back(state, silent_ms);
  };

  auto source = std::make_unique<fake_source>(audio_format{48000, 2, 16});
  fake_source* capture = source.get();
  audio_hub hub(std::move(source), options);

  int packets = 0;
  int resumes = 0;
  std::vector<std::size_t> sizes;
  std::string error;
  CHECK(hub.subscribe(
            [&](audio_pcm_ptr pcm, bool resume) {
              ++packets;
              if (resume) ++resumes;
              sizes.push_back(pcm->size());
            },
            error) != 0);

  const auto silent = constant_pcm(480, 2, 0);
  const auto loud = constant_pcm(480, 2, 8000);

  // The first N-1 quiet packets still reach the viewer; the Nth closes the
  // gate and is not sent.
  clock = 1000;
  capture->emit_samples(silent);
  clock = 1010;
  capture->emit_samples(silent);
  CHECK(packets == 2);
  clock = 1020;
  capture->emit_samples(silent);
  CHECK(packets == 2);
  CHECK(logs.size() == 1);
  CHECK(logs[0].first == "muted");
  CHECK(logs[0].second == 20);

  // Silence stays suppressed for as long as it lasts.
  clock = 1100;
  capture->emit_samples(silent);
  clock = 1150;
  capture->emit_samples(silent);
  CHECK(packets == 2);
  CHECK(resumes == 0);

  // Any audible packet resumes immediately and carries the marker. silent_ms
  // is measured from the first quiet packet of the stretch, not from the
  // moment the gate closed.
  clock = 1200;
  capture->emit_samples(loud);
  CHECK(packets == 3);
  CHECK(resumes == 1);
  CHECK(sizes.back() == 480 * 2);
  CHECK(logs.size() == 2);
  CHECK(logs[1].first == "active");
  CHECK(logs[1].second == 200);

  const audio_hub_stats stats = hub.take_stats();
  CHECK(stats.sent_packets == 3);
  CHECK(stats.muted_packets == 3);
  CHECK(stats.sample_rate == 48000);
  CHECK(stats.channels == 2);
  CHECK(stats.listeners == 1);
  CHECK(stats.kbps > 0.0);
}

void test_balanced_downsample() {
  audio_hub_options options;
  options.quality = "balanced";
  uint64_t clock = 0;
  options.now_ms = [&clock] { return clock; };

  auto source = std::make_unique<fake_source>(audio_format{48000, 2, 16});
  fake_source* capture = source.get();
  audio_hub hub(std::move(source), options);

  audio_pcm_ptr last;
  int resumes = 0;
  std::string error;
  CHECK(hub.subscribe(
            [&](audio_pcm_ptr pcm, bool resume) {
              last = pcm;
              if (resume) ++resumes;
            },
            error) != 0);

  const audio_format out = hub.format();
  CHECK(out.sample_rate == 24000);
  CHECK(out.channels == 1);
  CHECK(out.bits == 16);

  // 2 frames are averaged per output sample, and the stereo pair is mixed to
  // mono: (1000 + 3000) / 2 = 2000 is the expected amplitude.
  clock = 10;
  capture->emit_samples(stereo_pcm(480, 1000, 3000));
  CHECK(last != nullptr);
  CHECK(last->size() == 240);
  for (int16_t sample : *last) CHECK(sample == 2000);

  const audio_hub_stats stats = hub.take_stats();
  CHECK(stats.sample_rate == 24000);
  CHECK(stats.channels == 1);
  CHECK(stats.sent_packets == 1);
  CHECK(resumes == 0);
}

void test_low_downsample_and_quality_change() {
  audio_hub_options options;
  options.quality = "high";
  uint64_t clock = 0;
  options.now_ms = [&clock] { return clock; };

  auto source = std::make_unique<fake_source>(audio_format{48000, 2, 16});
  fake_source* capture = source.get();
  audio_hub hub(std::move(source), options);

  audio_pcm_ptr last;
  bool last_resume = false;
  std::string error;
  CHECK(hub.subscribe(
            [&](audio_pcm_ptr pcm, bool resume) {
              last = pcm;
              last_resume = resume;
            },
            error) != 0);

  clock = 10;
  capture->emit_samples(stereo_pcm(480, 1000, 3000));
  CHECK(last != nullptr);
  CHECK(last->size() == 480 * 2);
  CHECK(!last_resume);

  // Changing the quality is a format change: the next audible packet must
  // announce it with a resume marker, so the viewer can rebuild its player.
  CHECK(hub.set_quality("low"));
  CHECK(hub.format().sample_rate == 16000);
  CHECK(hub.format().channels == 1);
  clock = 20;
  capture->emit_samples(stereo_pcm(480, 1000, 3000));
  CHECK(last != nullptr);
  CHECK(last->size() == 160);
  CHECK(last_resume);
  for (int16_t sample : *last) CHECK(sample == 2000);

  // An unknown tier is refused rather than silently changing the sample rate.
  CHECK(!hub.set_quality("lossless"));
  CHECK(hub.quality() == "low");
}

void test_shared_buffer() {
  audio_hub_options options;
  options.quality = "high";

  auto source = std::make_unique<fake_source>(audio_format{48000, 2, 16});
  fake_source* capture = source.get();
  audio_hub hub(std::move(source), options);

  audio_pcm_ptr first;
  audio_pcm_ptr second;
  std::string error;
  CHECK(hub.subscribe(
            [&](audio_pcm_ptr pcm, bool) { first = pcm; }, error) != 0);
  CHECK(hub.subscribe(
            [&](audio_pcm_ptr pcm, bool) { second = pcm; }, error) != 0);

  capture->emit_samples(constant_pcm(120, 2, 4000));
  CHECK(first != nullptr);
  CHECK(second != nullptr);
  // Both listeners must receive the same allocation, not one copy each.
  CHECK(first.get() == second.get());
  CHECK(!first.owner_before(second));
  CHECK(!second.owner_before(first));
}

}  // namespace

int main() {
  test_silence_gate();
  test_balanced_downsample();
  test_low_downsample_and_quality_change();
  test_shared_buffer();
  if (failures != 0) {
    std::fprintf(stderr, "%d audio test(s) failed\n", failures);
    return 1;
  }
  std::printf("audio tests passed\n");
  return 0;
}
