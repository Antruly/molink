#include "security/ip.h"

#include <cstdlib>
#include <cstring>

#include "security/cn_ranges.h"
#include "util.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace molink {
namespace {

constexpr std::uint8_t kMaxPrefixV4 = 32;
constexpr std::uint8_t kMaxPrefixV6 = 128;

bool parse_v4(const std::string& text, ip_addr& out) {
  std::uint8_t bytes[4] = {0, 0, 0, 0};
  if (inet_pton(AF_INET, text.c_str(), bytes) != 1) return false;
  out.family = ip_family::v4;
  out.bytes.fill(0);
  std::memcpy(out.bytes.data(), bytes, 4);
  return true;
}

bool parse_v6(const std::string& text, ip_addr& out) {
  std::uint8_t bytes[16] = {};
  if (inet_pton(AF_INET6, text.c_str(), bytes) != 1) return false;
  out.family = ip_family::v6;
  std::memcpy(out.bytes.data(), bytes, 16);
  // Normalize ::ffff:a.b.c.d to plain IPv4.
  bool mapped = true;
  for (int i = 0; i < 10; ++i) {
    if (out.bytes[static_cast<std::size_t>(i)] != 0) {
      mapped = false;
      break;
    }
  }
  if (mapped && out.bytes[10] == 0xff && out.bytes[11] == 0xff) {
    ip_addr v4;
    v4.family = ip_family::v4;
    v4.bytes.fill(0);
    for (int i = 0; i < 4; ++i) {
      v4.bytes[static_cast<std::size_t>(i)] = out.bytes[12 + i];
    }
    out = v4;
  }
  return true;
}

std::uint32_t v4_number(const ip_addr& address) {
  return (static_cast<std::uint32_t>(address.bytes[0]) << 24) |
         (static_cast<std::uint32_t>(address.bytes[1]) << 16) |
         (static_cast<std::uint32_t>(address.bytes[2]) << 8) |
         static_cast<std::uint32_t>(address.bytes[3]);
}

bool within(const ip_addr& address, const char* cidr) {
  cidr_block block;
  if (!parse_cidr(cidr, block, nullptr)) return false;
  return cidr_contains(block, address);
}

}  // namespace

bool parse_ip(const std::string& text, ip_addr& out, std::string* error) {
  const std::string value = trim(text);
  if (value.empty()) {
    if (error != nullptr) *error = "empty IP address";
    return false;
  }
  const bool ok = value.find(':') == std::string::npos ? parse_v4(value, out)
                                                       : parse_v6(value, out);
  if (!ok && error != nullptr) *error = "invalid IP address: " + value;
  return ok;
}

bool parse_cidr(const std::string& text, cidr_block& out, std::string* error) {
  const std::string value = trim(text);
  if (value.empty()) {
    if (error != nullptr) *error = "empty CIDR";
    return false;
  }
  const std::size_t slash = value.find('/');
  const std::string address_text =
      slash == std::string::npos ? value : value.substr(0, slash);
  ip_addr address;
  if (!parse_ip(address_text, address, error)) return false;

  int prefix = address.family == ip_family::v4 ? kMaxPrefixV4 : kMaxPrefixV6;
  if (slash != std::string::npos) {
    const std::string prefix_text = value.substr(slash + 1);
    char* end = nullptr;
    const long parsed = std::strtol(prefix_text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed < 0 ||
        parsed > (address.family == ip_family::v4 ? kMaxPrefixV4
                                                  : kMaxPrefixV6)) {
      if (error != nullptr) *error = "invalid CIDR prefix: " + value;
      return false;
    }
    prefix = static_cast<int>(parsed);
  }

  out.network = address;
  out.prefix = static_cast<std::uint8_t>(prefix);
  // Zero the host bits so equality and display are canonical.
  const std::size_t whole = out.prefix / 8;
  const std::size_t bits = out.prefix % 8;
  for (std::size_t i = whole + (bits == 0 ? 0 : 1); i < 16; ++i) {
    out.network.bytes[i] = 0;
  }
  if (bits != 0 && whole < 16) {
    const std::uint8_t mask = static_cast<std::uint8_t>(0xff << (8 - bits));
    out.network.bytes[whole] &= mask;
  }
  return true;
}

std::string canonical_ip(const ip_addr& address) {
  char buffer[INET6_ADDRSTRLEN] = {};
  if (address.family == ip_family::v4) {
    if (inet_ntop(AF_INET, address.bytes.data(), buffer, sizeof(buffer)) ==
        nullptr) {
      return std::string();
    }
  } else if (inet_ntop(AF_INET6, address.bytes.data(), buffer,
                       sizeof(buffer)) == nullptr) {
    return std::string();
  }
  return buffer;
}

std::string canonical_cidr(const cidr_block& block) {
  return canonical_ip(block.network) + "/" + std::to_string(block.prefix);
}

bool cidr_contains(const cidr_block& block, const ip_addr& address) {
  if (block.network.family != address.family) return false;
  const std::size_t whole = block.prefix / 8;
  const std::size_t bits = block.prefix % 8;
  if (whole > 0 &&
      std::memcmp(block.network.bytes.data(), address.bytes.data(), whole) != 0) {
    return false;
  }
  if (bits != 0 && whole < 16) {
    const std::uint8_t mask = static_cast<std::uint8_t>(0xff << (8 - bits));
    if ((block.network.bytes[whole] & mask) !=
        (address.bytes[whole] & mask)) {
      return false;
    }
  }
  return true;
}

bool is_loopback(const ip_addr& address) {
  if (address.family == ip_family::v4) return address.bytes[0] == 127;
  for (std::size_t i = 0; i < 15; ++i) {
    if (address.bytes[i] != 0) return false;
  }
  return address.bytes[15] == 1;
}

bool is_private(const ip_addr& address) {
  if (address.family == ip_family::v4) {
    const std::uint32_t value = v4_number(address);
    // 10/8, 127/8, 169.254/16, 172.16/12, 192.168/16, 100.64/10.
    if ((value & 0xff000000u) == 0x0a000000u) return true;
    if ((value & 0xff000000u) == 0x7f000000u) return true;
    if ((value & 0xffff0000u) == 0xa9fe0000u) return true;
    if ((value & 0xfff00000u) == 0xac100000u) return true;
    if ((value & 0xffff0000u) == 0xc0a80000u) return true;
    if ((value & 0xffc00000u) == 0x64400000u) return true;
    return false;
  }
  // ::1/128, fc00::/7, fe80::/10.
  if (is_loopback(address)) return true;
  if ((address.bytes[0] & 0xfe) == 0xfc) return true;
  if (address.bytes[0] == 0xfe && (address.bytes[1] & 0xc0) == 0x80) return true;
  return false;
}

ip_category classify_ip(const ip_addr& address) {
  if (is_private(address)) return ip_category::private_net;
  if (address.family == ip_family::v4 && cn_ipv4_contains(v4_number(address))) {
    return ip_category::mainland_china;
  }
  return ip_category::overseas;
}

bool policy_allows(const ip_policy& policy, const ip_addr& address) {
  if (is_loopback(address)) return true;
  for (const cidr_block& block : policy.custom_deny) {
    if (cidr_contains(block, address)) return false;
  }
  for (const cidr_block& block : policy.custom_allow) {
    if (cidr_contains(block, address)) return true;
  }
  switch (classify_ip(address)) {
    case ip_category::private_net:
      return policy.allow_private;
    case ip_category::mainland_china:
      return policy.allow_mainland_china;
    case ip_category::overseas:
      return policy.allow_overseas;
  }
  return false;
}

bool resolve_client_ip(const std::string& peer, const std::string& forwarded_for,
                       const std::vector<cidr_block>& trusted_proxies,
                       ip_addr& out) {
  ip_addr peer_addr;
  if (!parse_ip(peer, peer_addr, nullptr)) return false;
  if (trusted_proxies.empty() || forwarded_for.empty()) {
    out = peer_addr;
    return true;
  }

  bool trusted = false;
  for (const cidr_block& block : trusted_proxies) {
    if (cidr_contains(block, peer_addr)) {
      trusted = true;
      break;
    }
  }
  if (!trusted) {
    out = peer_addr;
    return true;
  }

  std::vector<std::string> hops = split(forwarded_for, ',');
  if (hops.size() > 16) return false;
  ip_addr current = peer_addr;
  for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
    ip_addr hop;
    if (!parse_ip(trim(*it), hop, nullptr)) return false;
    bool hop_trusted = false;
    for (const cidr_block& block : trusted_proxies) {
      if (cidr_contains(block, hop)) {
        hop_trusted = true;
        break;
      }
    }
    if (!hop_trusted) {
      out = hop;
      return true;
    }
    current = hop;
  }
  out = current;
  return true;
}

}  // namespace molink
