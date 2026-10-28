#pragma once

#include <memory>
#include <string>
#include <vector>

#include "capture/av1_encoder.h"

namespace molink {

/// True when the NVIDIA encode API and an AV1-capable encoder are present.
/// The probe is cached; the first call may create a temporary D3D11/CUDA
/// device.
bool nvenc_av1_available();

/// True when NVENC can encode H.264 on this machine. Much wider than AV1: it
/// is what a viewer without an AV1 decoder falls back to.
bool nvenc_h264_available();

/// The NVENC device list (empty when unavailable).
std::vector<av1_encoder_device> nvenc_av1_devices();

/// NVENC preset number used for a speed name (1 = fastest, 7 = slowest).
int nvenc_preset_for(const std::string& speed);

/// Null when NVENC is unavailable or the session cannot be created.
std::shared_ptr<av1_encoder> make_nvenc_av1_encoder(
    const av1_encoder_config& config, std::string* error);

/// NVENC low-delay AV1 stream: one keyframe followed by P frames that only
/// carry what changed. Encoding a 1080p screen on the GPU costs a few
/// milliseconds per frame where SVT-AV1 needs hundreds, which is what keeps
/// the stream path from collapsing to a handful of frames per second on a CPU.
///
/// Null when NVENC is unavailable, the codec is not supported by this GPU, or
/// the session cannot be created.
std::shared_ptr<av1_stream_encoder> make_nvenc_stream_encoder(
    const av1_encoder_config& config, video_codec codec, std::string* error);

/// AV1-only convenience wrapper.
std::shared_ptr<av1_stream_encoder> make_nvenc_av1_stream_encoder(
    const av1_encoder_config& config, std::string* error);

}  // namespace molink
