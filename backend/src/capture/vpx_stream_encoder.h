#pragma once

#include <memory>
#include <string>

#include "capture/av1_encoder.h"

namespace molink {

/// True when this build found libvpx and can encode VP8/VP9.
bool vpx_stream_available();

/// Builds a libvpx-backed low-delay VP9 or VP8 stream encoder. `config.device`
/// is ignored (libvpx is CPU only); `config.threads` is passed to the encoder.
/// Returns nullptr and fills `error` when the encoder cannot be created.
std::shared_ptr<av1_stream_encoder> make_vpx_stream_encoder(
    const av1_encoder_config& config, video_codec codec, std::string* error);

}  // namespace molink
