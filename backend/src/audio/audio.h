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

/// Interleaved 16-bit PCM at audio_hub::format(). One buffer is shared by
/// every listener of a packet, so a host with N viewers does not copy (or
/// hold) N copies of the same samples.
using audio_pcm = std::vector<int16_t>;
using audio_pcm_ptr = std::shared_ptr<const audio_pcm>;

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

/// Tunables for the hub. The defaults are the shipping behaviour; the clock
/// and the log sink are injectable so the gate can be tested without sleeping
/// or writing diagnostics.
struct audio_hub_options {
  /// "high" keeps the loopback format (48 kHz stereo), "balanced" halves the
  /// sample rate and mixes to mono (24 kHz), "low" takes 16 kHz mono.
  std::string quality = "balanced";
  /// Consecutive silent packets before the gate stops sending.
  int silence_packets = 5;
  /// RMS below this level (dBFS) counts as silence.
  double silence_dbfs = -60.0;
  /// Monotonic milliseconds, used only for the silent_ms field in the log.
  std::function<uint64_t()> now_ms;
  /// Called on a gate state change: "muted" or "active", plus how long the
  /// silence lasted (or the muted stretch lasted, on resume).
  std::function<void(const char* state, uint64_t silent_ms)> gate_log;
};

struct audio_hub_stats {
  /// Audible packets fanned out to listeners.
  uint64_t sent_packets = 0;
  /// Packets suppressed by the silence gate.
  uint64_t muted_packets = 0;
  /// Payload bandwidth in the window, counting one copy per listener.
  double kbps = 0.0;
  int sample_rate = 48000;
  int channels = 2;
  int listeners = 0;
};

/// Fans loopback PCM out to interested sessions. Capture runs only while at
/// least one session is listening. A silence gate stops quiet packets and a
/// quality level mixes/downsamples them before they are encoded.
class audio_hub {
 public:
  /// @p resume is true on the first audible packet after a muted stretch:
  /// the viewer resets its jitter buffer instead of treating the gap as loss.
  using listener = std::function<void(audio_pcm_ptr pcm, bool resume)>;

  explicit audio_hub(std::unique_ptr<audio_source> source,
                     audio_hub_options options = {});
  ~audio_hub();

  bool available() const;
  /// The format listeners receive: the source format after the quality
  /// downmix, not necessarily the loopback device's format.
  audio_format format() const;

  /// "high" | "balanced" | "low". Unknown values are rejected.
  bool set_quality(const std::string& quality);
  std::string quality() const;

  /// Counters since the previous call, with the rate over that window.
  audio_hub_stats take_stats();

  /// Returns a subscription token, or 0 on failure.
  int subscribe(listener fn, std::string& err);
  void unsubscribe(int token);

 private:
  using gate_log_fn = std::function<void(const char* state, uint64_t silent_ms)>;

  void dispatch(std::vector<uint8_t> pcm);
  /// Effective output format for @p source under @p quality.
  static audio_format output_format(const audio_format& source,
                                    const std::string& quality);
  /// Packets grouped per output sample (1 when no downsampling is needed).
  static int downsample_ratio(int source_rate, const std::string& quality);
  uint64_t now_ms() const;

  std::unique_ptr<audio_source> source_;
  mutable std::mutex mutex_;
  std::vector<std::pair<int, listener>> listeners_;
  int next_token_ = 1;
  bool running_ = false;

  audio_hub_options options_;
  std::string quality_;
  /// Silence gate state, owned by the capture thread but guarded because
  /// take_stats() and set_quality() run on the app loop.
  int silent_packets_ = 0;
  bool muted_ = false;
  /// Set by a quality change so the next audible packet carries a resume
  /// marker, which also tells the viewer the new rate/channels.
  bool force_resume_ = false;
  uint64_t silence_started_ms_ = 0;
  /// Downmix accumulator carried across packet boundaries so an odd-sized
  /// packet does not lose samples or drift the rate.
  int64_t mix_accum_ = 0;
  int mix_frames_ = 0;
  /// Latest source format seen while capturing, for stats after a device
  /// change; format() probes the source when capture is idle.
  audio_format last_source_format_;

  uint64_t last_stats_ms_ = 0;
  uint64_t stats_sent_packets_ = 0;
  uint64_t stats_muted_packets_ = 0;
  uint64_t stats_bytes_ = 0;
};

}  // namespace molink
