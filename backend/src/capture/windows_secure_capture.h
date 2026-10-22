#pragma once

#include <memory>

#include "capture/capture.h"

namespace molink {

/// Capture the active session's input desktop through molink-helper. This is
/// what makes the Winlogon/UAC desktop visible; it falls back to DXGI when the
/// helper is not running. Returns nullptr outside Windows.
std::unique_ptr<capture_source> make_windows_secure_capture(
    const capture_options& options);

}  // namespace molink
