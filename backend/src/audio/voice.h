#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace molink {

/// Two-way voice for a call: the host microphone is sent to the caller and
/// the caller's PCM is played on the host speakers. Fixed format on the wire:
/// 48 kHz, mono, little-endian 16-bit PCM.
class voice_engine {
 public:
  using sink = std::function<void(std::vector<uint8_t>)>;
  virtual ~voice_engine() = default;
  virtual bool microphone_available() const = 0;
  virtual bool start(sink capture, std::string& err) = 0;
  virtual void play(const uint8_t* pcm, std::size_t len) = 0;
  virtual void stop() = 0;
};

/// Windows only; returns nullptr elsewhere.
std::unique_ptr<voice_engine> make_voice_engine(std::string& err);

}  // namespace molink
