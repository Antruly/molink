#pragma once

#include <atomic>
#include <memory>

#include "supervisor_policy.h"

namespace molink {
namespace supervisor {

/// The real Windows side of supervision: launches molink-core.exe, watches
/// it, and reuses the core's existing app.exit diagnostic convention.
std::unique_ptr<backend> make_core_supervisor(std::atomic<bool>& running,
                                              const restart_policy& policy);

}  // namespace supervisor
}  // namespace molink
