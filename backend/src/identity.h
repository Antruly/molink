#pragma once

#include <string>

#include "config.h"

namespace molink {

struct device_identity {
  std::string device_id;  // ABC-DEF-GHI, stable per installation
  std::string token;      // verification code, stable unless overridden
};

/// Loads (or creates) the persistent device id and access token.
///
/// The token from --token or MOLINK_TOKEN wins and is never written to disk.
/// Otherwise a 32-hex token is generated once and reused, so the verification
/// code shown in the UI stays stable across restarts.
device_identity load_identity(const core_config& config, std::string& err);

/// Platform configuration directory, or empty when it cannot be determined.
std::string config_directory();

/// Idle-lock timeout persisted from the UI. Returns @p fallback when unset.
int load_lock_timeout_setting(int fallback);
void save_lock_timeout_setting(int seconds);

}  // namespace molink
