#pragma once

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "capture/av1_encoder.h"

namespace molink {

/// Runs a GPU stream encoder first and keeps a CPU encoder behind it.
///
/// A GPU session can fail mid-stream (driver reset, device lost). Dropping the
/// frame would stall the decoder instead of degrading, so the picture is
/// re-encoded on the CPU, and the CPU stream starts with a keyframe because
/// its reference chain is not the GPU's. The GPU is dropped only after
/// repeated failures, so one bad frame cannot cost the session its hardware
/// encoder. The CPU encoder is built on demand, so a healthy GPU session never
/// pays for SVT's worker threads.
class fallback_av1_stream_encoder final : public av1_stream_encoder {
 public:
  using cpu_factory = std::function<std::shared_ptr<av1_stream_encoder>()>;

  static constexpr int kGpuFailureLimit = 3;

  fallback_av1_stream_encoder(std::shared_ptr<av1_stream_encoder> gpu,
                              cpu_factory make_cpu)
      : gpu_(std::move(gpu)), make_cpu_(std::move(make_cpu)) {}

  bool encode_stream(const uint8_t* bgra, std::size_t stride, int width,
                     int height, int qp, bool keyframe,
                     std::vector<uint8_t>& out, bool& out_keyframe,
                     std::string& error) override {
    if (gpu_) {
      std::string gpu_error;
      if (gpu_->encode_stream(bgra, stride, width, height, qp, keyframe, out,
                              out_keyframe, gpu_error)) {
        failures_ = 0;
        return true;
      }
      if (!reported_) {
        reported_ = true;
        std::fprintf(stderr, "av1 stream: nvenc failed: %s\n",
                     gpu_error.c_str());
      }
      if (++failures_ >= kGpuFailureLimit) {
        std::fprintf(stderr,
                     "av1 stream: nvenc dropped, continuing on the CPU\n");
        gpu_.reset();
      }
      error = gpu_error;
      // Fall through so this picture still reaches the viewer.
    }
    if (!cpu_) {
      cpu_ = make_cpu_ ? make_cpu_() : nullptr;
      if (!cpu_) {
        if (error.empty()) error = "av1 stream: no CPU encoder to fall back to";
        return false;
      }
      // The factory captured the configuration from when this wrapper was
      // built; carry a bitrate change that happened since into the new CPU
      // encoder before its first picture.
      if (has_kbps_) cpu_->set_bitrate(static_cast<uint32_t>(kbps_));
      // A fresh CPU stream has no reference picture, whatever the caller's
      // keyframe flag says.
      cpu_keyframe_ = true;
    }
    const bool force = keyframe || cpu_keyframe_;
    if (!cpu_->encode_stream(bgra, stride, width, height, qp, force, out,
                             out_keyframe, error)) {
      return false;
    }
    cpu_keyframe_ = false;
    return true;
  }

  bool set_bitrate(uint32_t kbps) override {
    kbps_ = static_cast<int>(kbps);
    has_kbps_ = true;
    if (gpu_) return gpu_->set_bitrate(kbps);
    if (cpu_) return cpu_->set_bitrate(kbps);
    // Nothing built yet: the lazy CPU encoder applies this when it starts.
    return true;
  }

  /// True while the GPU is still in the chain (diagnostics and tests).
  bool has_gpu() const { return gpu_ != nullptr; }
  bool has_cpu() const { return cpu_ != nullptr; }

 private:
  std::shared_ptr<av1_stream_encoder> gpu_;
  cpu_factory make_cpu_;
  std::shared_ptr<av1_stream_encoder> cpu_;
  int failures_ = 0;
  bool reported_ = false;
  bool cpu_keyframe_ = false;
  int kbps_ = 0;
  bool has_kbps_ = false;
};

}  // namespace molink
