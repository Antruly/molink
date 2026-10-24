#pragma once

#include <cstdint>

namespace molink {

/// True when the IPv4 address (host order) falls inside the embedded APNIC
/// mainland-China allocation snapshot.
bool cn_ipv4_contains(std::uint32_t address);

}  // namespace molink
