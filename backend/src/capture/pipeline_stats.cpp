#include "capture/pipeline_stats.h"

#include <algorithm>
#include <utility>

namespace molink {

const char* capture_trigger_name(capture_trigger trigger) {
  switch (trigger) {
    case capture_trigger::initial:
      return "initial";
    case capture_trigger::timer:
      return "timer";
    case capture_trigger::change:
      return "change";
    case capture_trigger::input:
      return "input";
    case capture_trigger::keyframe:
      return "keyframe";
    case capture_trigger::stream:
      return "stream";
    case capture_trigger::count:
      break;
  }
  return "other";
}

uint64_t percentile95(std::vector<uint64_t> samples) {
  if (samples.empty()) return 0;
  std::sort(samples.begin(), samples.end());
  // Nearest-rank: the sample at ceil(0.95 * n), clamped to the last index.
  std::size_t rank =
      static_cast<std::size_t>((samples.size() * 95 + 99) / 100);
  if (rank == 0) rank = 1;
  if (rank > samples.size()) rank = samples.size();
  return samples[rank - 1];
}

void pipeline_monitor::add_sample(std::vector<uint64_t>& samples,
                                  uint64_t value) {
  if (samples.size() < kMaxSamples) {
    samples.push_back(value);
    return;
  }
  // Keep the distribution bounded without a windowed reservoir: drop every
  // other sample once, which halves the stored count and leaves the shape.
  std::size_t write = 0;
  for (std::size_t read = 0; read < samples.size(); read += 2) {
    samples[write++] = samples[read];
  }
  samples.resize(write);
  samples.push_back(value);
}

uint64_t pipeline_monitor::now_us() const {
  return clock_ ? clock_() : pipeline_now_us();
}

void pipeline_monitor::note_capture(capture_trigger trigger,
                                    uint64_t trigger_us,
                                    uint64_t capture_start_us,
                                    uint64_t capture_done_us,
                                    uint64_t encode_start_us,
                                    uint64_t encode_end_us,
                                    uint64_t input_delay_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_) {
    started_ = true;
    window_start_us_ = now_us();
  }
  ++captured_;
  const std::size_t index = static_cast<std::size_t>(trigger);
  if (index < static_cast<std::size_t>(capture_trigger::count)) {
    ++triggers_[index];
  }
  if (trigger_us > 0 && capture_start_us >= trigger_us) {
    add_sample(trigger_delays_us_, capture_start_us - trigger_us);
  }
  if (capture_done_us > 0 && encode_end_us >= encode_start_us) {
    ++encoded_;
    add_sample(encode_us_, encode_end_us - encode_start_us);
  }
  if (input_delay_us > 0) add_sample(input_delays_us_, input_delay_us);
}

void pipeline_monitor::note_sent(uint64_t capture_done_us, uint64_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_) {
    started_ = true;
    window_start_us_ = now_us();
  }
  ++sent_;
  bytes_ += bytes;
  const uint64_t now = now_us();
  if (capture_done_us > 0 && now >= capture_done_us) {
    add_sample(ages_us_, now - capture_done_us);
  }
}

void pipeline_monitor::note_sent_bytes(uint64_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  bytes_ += bytes;
}

void pipeline_monitor::note_dropped(uint64_t frames) {
  std::lock_guard<std::mutex> lock(mutex_);
  dropped_ += frames;
}

void pipeline_monitor::note_send_state(uint64_t sent_seq, uint64_t latest_seq) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (latest_seq > sent_seq) {
    queue_depth_ = (std::max)(queue_depth_, latest_seq - sent_seq);
  }
}

bool pipeline_monitor::due_locked(uint64_t now) const {
  if (!started_) return false;
  return now >= window_start_us_ && now - window_start_us_ >= window_us_;
}

bool pipeline_monitor::due() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return due_locked(now_us());
}

pipeline_snapshot pipeline_monitor::take_locked(uint64_t now) {
  pipeline_snapshot out;
  out.captured = captured_;
  out.encoded = encoded_;
  out.sent = sent_;
  out.dropped = dropped_;
  out.queue_depth = queue_depth_;
  out.bytes = bytes_;
  out.age_p95_us = percentile95(std::move(ages_us_));
  out.encode_p95_us = percentile95(std::move(encode_us_));
  out.trigger_delay_p95_us = percentile95(std::move(trigger_delays_us_));
  out.input_delay_p95_us = percentile95(std::move(input_delays_us_));
  for (std::size_t i = 0;
       i < static_cast<std::size_t>(capture_trigger::count); ++i) {
    out.triggers[i] = triggers_[i];
  }
  out.window_us = started_ && now >= window_start_us_
                      ? now - window_start_us_
                      : window_us_;
  const double seconds = out.window_us > 0
                             ? static_cast<double>(out.window_us) / 1000000.0
                             : 0.0;
  out.kbps = seconds > 0.0 ? static_cast<double>(bytes_) * 8.0 / seconds / 1000.0
                           : 0.0;

  captured_ = 0;
  encoded_ = 0;
  sent_ = 0;
  dropped_ = 0;
  queue_depth_ = 0;
  bytes_ = 0;
  for (uint64_t& count : triggers_) count = 0;
  ages_us_.clear();
  encode_us_.clear();
  trigger_delays_us_.clear();
  input_delays_us_.clear();
  started_ = true;
  window_start_us_ = now;
  return out;
}

pipeline_snapshot pipeline_monitor::take() {
  std::lock_guard<std::mutex> lock(mutex_);
  return take_locked(now_us());
}

pipeline_snapshot pipeline_monitor::peek() const {
  std::lock_guard<std::mutex> lock(mutex_);
  pipeline_snapshot out;
  out.captured = captured_;
  out.encoded = encoded_;
  out.sent = sent_;
  out.dropped = dropped_;
  out.queue_depth = queue_depth_;
  out.bytes = bytes_;
  out.age_p95_us = percentile95(ages_us_);
  out.encode_p95_us = percentile95(encode_us_);
  out.trigger_delay_p95_us = percentile95(trigger_delays_us_);
  out.input_delay_p95_us = percentile95(input_delays_us_);
  for (std::size_t i = 0;
       i < static_cast<std::size_t>(capture_trigger::count); ++i) {
    out.triggers[i] = triggers_[i];
  }
  const uint64_t now = now_us();
  out.window_us = started_ && now >= window_start_us_
                      ? now - window_start_us_
                      : window_us_;
  const double seconds = out.window_us > 0
                             ? static_cast<double>(out.window_us) / 1000000.0
                             : 0.0;
  out.kbps = seconds > 0.0
                 ? static_cast<double>(bytes_) * 8.0 / seconds / 1000.0
                 : 0.0;
  return out;
}

pipeline_monitor& video_pipeline() {
  static pipeline_monitor monitor;
  return monitor;
}

}  // namespace molink
