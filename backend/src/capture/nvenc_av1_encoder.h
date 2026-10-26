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

/// The NVENC device list (empty when unavailable).
std::vector<av1_encoder_device> nvenc_av1_devices();

/// NVENC preset number used for a speed name (1 = fastest, 7 = slowest).
int nvenc_preset_for(const std::string& speed);

/// Null when NVENC is unavailable or the session cannot be created.
std::shared_ptr<av1_encoder> make_nvenc_av1_encoder(
    const av1_encoder_config& config, std::string* error);

}  // namespace molink
