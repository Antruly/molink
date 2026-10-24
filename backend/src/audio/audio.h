#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace molink {

struct audio_format {
  int sample_rate = 48000;
  int channels = 2;
  int bits = 16;
};

/// Platform loopback capture. On Windows this is WASAPI render loopback, so it
/// carries whatever the machine is playing.
class audio_source {
 public:
  /// Interleaved little-endian PCM in format().
  using sink = std::function<void(std::vector<uint8_t>)>;
  virtual ~audio_source() = default;
  virtual bool available() const = 0;
  virtual audio_format format() const = 0;
  virtual bool start(sink callback, std::string& err) = 0;
  virtual void stop() = 0;
};

std::unique_ptr<audio_source> make_audio_source(std::string& err);

/// Fans loopback PCM out to interested sessions. Capture runs only while at
/// least one session is listening.
class audio_hub {
 public:
  using listener = std::function<void(std::vector<uint8_t>)>;

  explicit audio_hub(std::unique_ptr<audio_source> source);
  ~audio_hub();

  bool available() const;
  audio_format format() const;

  /// Returns a subscription token, or 0 on failure.
  int subscribe(listener fn, std::string& err);
  void unsubscribe(int token);

 private:
  void dispatch(std::vector<uint8_t> pcm);

  std::unique_ptr<audio_source> source_;
  std::mutex mutex_;
  std::vector<std::pair<int, listener>> listeners_;
  int next_token_ = 1;
  bool running_ = false;
};

}  // namespace molink
