#pragma once

#include <cstdint>
#include <string>

namespace molink {

/// Loopback is the only default binding; the core must not be reachable from
/// the LAN unless the operator asked for it. Exposed here so the ready line and
/// the HTTP handlers describe the same thing.
bool is_loopback_host(const std::string& host);

/// Uppercase base32-ish alphabet without I/L/O/0/1, so a session id can be read
/// aloud or typed from a screenshot without ambiguity.
std::string make_session_public_id();

/// Monotonic milliseconds since an arbitrary origin.
uint64_t now_ms();

}  // namespace molink
