#pragma once

#include <memory>

#include "capture/capture.h"

namespace molink {

/// Capture the active session's input desktop through molink-helper. This is
/// what makes the Winlogon/UAC desktop visible; it falls back to DXGI when the
/// helper is not running. Returns nullptr outside Windows.
std::unique_ptr<capture_source> make_windows_secure_capture(
    const capture_options& options);

/// True when the console session's input desktop is not the normal "Default"
/// one -- the lock screen or a UAC secure desktop, which DXGI cannot capture.
/// Always false outside Windows.
bool secure_desktop_showing();

/// Asks the SYSTEM helper which desktop is in front. A normal user process
/// cannot always open the Winlogon desktop, so the helper's answer is the
/// reliable one; `reachable` reports whether it answered at all. Always false
/// outside Windows.
bool helper_secure_desktop(bool& reachable);

}  // namespace molink
