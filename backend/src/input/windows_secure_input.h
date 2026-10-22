#pragma once

#include <memory>

#include "input/input.h"

namespace molink {

/// Injects input into molink-helper's current desktop (Winlogon/UAC included).
/// Returns nullptr outside Windows or when the helper pipe is unavailable.
std::unique_ptr<input_injector> make_windows_secure_input();

}  // namespace molink
