#include "audio/audio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace molink {
namespace {

std::string normalize_quality(const std::string& quality) {
  if (quality == "high" || quality == "balanced" || quality == "low") {
    return quality;
  }
  return "balanced";
}

}  // namespace

audio_hub::audio_hub(std::unique_ptr<audio_source> source,
                     audio_hub_options options)
    : source_(std::move(source)),
      options_(std::move(options)),
      quality_(normalize_quality(options_.quality)) {
  if (options_.silence_packets < 1) options_.silence_packets = 1;
  if (source_) last_source_format_ = source_->format();
  last_stats_ms_ = now_ms();
}

audio_hub::~audio_hub() {
  bool stop = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.clear();
    stop = running_;
    running_ = false;
  }
  if (stop && source_) source_->stop();
}

bool audio_hub::available() const {
  return source_ != nullptr && source_->available();
}

audio_format audio_hub::format() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return output_format(last_source_format_, quality_);
}

bool audio_hub::set_quality(const std::string& quality) {
  if (quality != "high" && quality != "balanced" && quality != "low") {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (quality == quality_) return true;
  quality_ = quality;
  // A new format changes the meaning of every sample: start the gate and the
  // cross-packet accumulator clean, and make the next audible packet announce
  // the new format (the resume marker carries rate/channels).
  silent_packets_ = 0;
  muted_ = false;
  force_resume_ = true;
  mix_accum_ = 0;
  mix_frames_ = 0;
  return true;
}

std::string audio_hub::quality() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return quality_;
}

audio_format audio_hub::output_format(const audio_format& source,
                                      const std::string& quality) {
  audio_format out = source;
  out.bits = 16;
  if (quality == "high") return out;
  if (out.sample_rate <= 0) out.sample_rate = 48000;
  const int ratio = downsample_ratio(out.sample_rate, quality);
  out.sample_rate = out.sample_rate / ratio;
  out.channels = 1;
  return out;
}

int audio_hub::downsample_ratio(int source_rate, const std::string& quality) {
  const int wanted = quality == "low" ? 16000 : 24000;
  if (quality == "high" || source_rate <= wanted) return 1;
  int ratio = (source_rate + wanted / 2) / wanted;
  return ratio < 1 ? 1 : ratio;
}

uint64_t audio_hub::now_ms() const {
  if (options_.now_ms) return options_.now_ms();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

int audio_hub::subscribe(listener fn, std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!source_ || !source_->available()) {
    err = "audio capture is not available";
    return 0;
  }
  const int token = next_token_++;
  listeners_.emplace_back(token, std::move(fn));
  if (!running_) {
    std::string start_error;
    if (!source_->start(
            [this](std::vector<uint8_t> pcm) { dispatch(std::move(pcm)); },
            start_error)) {
      listeners_.pop_back();
      err = start_error.empty() ? "could not start audio capture" : start_error;
      return 0;
    }
    running_ = true;
    // The device may have changed since the probe; the gate and the
    // accumulator belong to the previous listener set.
    last_source_format_ = source_->format();
    silent_packets_ = 0;
    muted_ = false;
    force_resume_ = false;
    mix_accum_ = 0;
    mix_frames_ = 0;
    stats_sent_packets_ = 0;
    stats_muted_packets_ = 0;
    stats_bytes_ = 0;
    last_stats_ms_ = now_ms();
  }
  return token;
}

void audio_hub::unsubscribe(int token) {
  bool stop = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
      if (it->first == token) {
        listeners_.erase(it);
        break;
      }
    }
    if (listeners_.empty() && running_) {
      running_ = false;
      stop = true;
      silent_packets_ = 0;
      muted_ = false;
      force_resume_ = false;
      mix_accum_ = 0;
      mix_frames_ = 0;
    }
  }
  // stop() joins the capture thread; do it without the mutex because the
  // capture thread may still be delivering a callback.
  if (stop && source_) source_->stop();
}

audio_hub_stats audio_hub::take_stats() {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t now = now_ms();
  const uint64_t elapsed_ms =
      now >= last_stats_ms_ ? now - last_stats_ms_ : 0;
  audio_hub_stats stats;
  stats.sent_packets = stats_sent_packets_;
  stats.muted_packets = stats_muted_packets_;
  if (elapsed_ms > 0) {
    stats.kbps =
        static_cast<double>(stats_bytes_) * 8.0 / 1000.0 /
        (static_cast<double>(elapsed_ms) / 1000.0);
  }
  const audio_format out = output_format(last_source_format_, quality_);
  stats.sample_rate = out.sample_rate;
  stats.channels = out.channels;
  stats.listeners = static_cast<int>(listeners_.size());
  stats_sent_packets_ = 0;
  stats_muted_packets_ = 0;
  stats_bytes_ = 0;
  last_stats_ms_ = now;
  return stats;
}

void audio_hub::dispatch(std::vector<uint8_t> pcm) {
  if (pcm.size() < 2) return;

  // Decode once, and decide silence on the decoded samples so the gate sees
  // the same signal the downmix does.
  const std::size_t sample_count = pcm.size() / 2;
  std::vector<int16_t> input(sample_count);
  double sum_squares = 0.0;
  for (std::size_t i = 0; i < sample_count; ++i) {
    const uint16_t raw = static_cast<uint16_t>(
        static_cast<uint16_t>(pcm[i * 2]) |
        (static_cast<uint16_t>(pcm[i * 2 + 1]) << 8));
    const int16_t value = static_cast<int16_t>(raw);
    input[i] = value;
    const double sample = static_cast<double>(value);
    sum_squares += sample * sample;
  }
  const double rms =
      sample_count == 0 ? 0.0 : std::sqrt(sum_squares / sample_count);
  const double dbfs =
      rms > 0.0 ? 20.0 * std::log10(rms / 32768.0) : -1000.0;
  const bool silent = dbfs < options_.silence_dbfs;

  std::vector<listener> listeners;
  audio_pcm_ptr output;
  const char* gate_event = nullptr;
  uint64_t gate_silent_ms = 0;
  bool resume = false;
  bool deliver = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || listeners_.empty()) return;
    listeners.reserve(listeners_.size());
    for (const auto& item : listeners_) listeners.push_back(item.second);
    if (source_) last_source_format_ = source_->format();

    const uint64_t now = now_ms();
    bool send = false;
    if (silent) {
      if (silent_packets_ == 0) silence_started_ms_ = now;
      ++silent_packets_;
      if (!muted_ && silent_packets_ >= options_.silence_packets) {
        muted_ = true;
        gate_event = "muted";
        gate_silent_ms =
            now >= silence_started_ms_ ? now - silence_started_ms_ : 0;
        ++stats_muted_packets_;
      } else if (muted_) {
        ++stats_muted_packets_;
      } else {
        send = true;
      }
    } else {
      if (muted_ || force_resume_) {
        resume = true;
        force_resume_ = false;
        if (muted_) {
          gate_event = "active";
          gate_silent_ms =
              now >= silence_started_ms_ ? now - silence_started_ms_ : 0;
        }
        muted_ = false;
      }
      silent_packets_ = 0;
      silence_started_ms_ = 0;
      send = true;
    }

    if (send) {
      if (quality_ == "high") {
        output = std::make_shared<const audio_pcm>(std::move(input));
      } else {
        const audio_format source_format =
            last_source_format_.sample_rate > 0 ? last_source_format_
                                                : audio_format{};
        const int ratio =
            downsample_ratio(source_format.sample_rate, quality_);
        const int channels = source_format.channels > 0
                                 ? source_format.channels
                                 : 1;
        const std::size_t frames = input.size() / channels;
        audio_pcm mixed;
        mixed.reserve(frames / ratio + 1);
        const int64_t divisor = static_cast<int64_t>(ratio) * channels;
        for (std::size_t frame = 0; frame < frames; ++frame) {
          int64_t sum = 0;
          for (int channel = 0; channel < channels; ++channel) {
            sum += input[frame * channels + channel];
          }
          mix_accum_ += sum;
          ++mix_frames_;
          if (mix_frames_ >= ratio) {
            mixed.push_back(static_cast<int16_t>(mix_accum_ / divisor));
            mix_accum_ = 0;
            mix_frames_ = 0;
          }
        }
        output = std::make_shared<const audio_pcm>(std::move(mixed));
      }
      if (output && !output->empty()) {
        ++stats_sent_packets_;
        stats_bytes_ +=
            static_cast<uint64_t>(output->size()) * 2 *
            static_cast<uint64_t>(listeners.size());
        deliver = true;
      }
    }
  }

  if (gate_event != nullptr && options_.gate_log) {
    options_.gate_log(gate_event, gate_silent_ms);
  }
  if (!deliver || !output) return;
  for (const listener& fn : listeners) {
    if (fn) fn(output, resume);
  }
}

}  // namespace molink
