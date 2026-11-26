#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "supervisor_policy.h"

namespace molink {
namespace supervisor {

/// The real Windows side of supervision: launches molink-core.exe, watches
/// it, and reuses the core's existing app.exit diagnostic convention.
/// `service_name` selects the data directory and Global\ object names (see
/// service_paths.h); pass molink::service_paths::kDefaultServiceName when the
/// helper was started without --name.
std::unique_ptr<backend> make_core_supervisor(std::atomic<bool>& running,
                                              const restart_policy& policy,
                                              std::wstring service_name);

}  // namespace supervisor
}  // namespace molink
