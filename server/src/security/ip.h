#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace molink {

enum class ip_family { v4, v6 };

struct ip_addr {
  ip_family family = ip_family::v4;
  std::array<std::uint8_t, 16> bytes{};
};

struct cidr_block {
  ip_addr network;
  std::uint8_t prefix = 0;
};

bool parse_ip(const std::string& text, ip_addr& out, std::string* error = nullptr);
bool parse_cidr(const std::string& text, cidr_block& out,
                std::string* error = nullptr);
std::string canonical_ip(const ip_addr& address);
std::string canonical_cidr(const cidr_block& block);
bool cidr_contains(const cidr_block& block, const ip_addr& address);
bool is_loopback(const ip_addr& address);
bool is_private(const ip_addr& address);

enum class ip_category { private_net, mainland_china, overseas };

/// Category used by the default policy. IPv6 addresses are either private
/// (loopback/ULA/link-local) or overseas; the embedded table is IPv4.
ip_category classify_ip(const ip_addr& address);

struct ip_policy {
  bool allow_private = true;
  bool allow_mainland_china = true;
  bool allow_overseas = false;
  std::vector<cidr_block> custom_allow;
  std::vector<cidr_block> custom_deny;
};

/// Loopback is always allowed; `custom_deny` wins over `custom_allow`, which
/// wins over the category switches.
bool policy_allows(const ip_policy& policy, const ip_addr& address);

/// Resolve the effective client address from a peer and an X-Forwarded-For
/// value. When `trusted_proxies` is empty the header is ignored. Returns false
/// when the header is present but malformed; the caller should then fall back
/// to the peer address.
bool resolve_client_ip(const std::string& peer, const std::string& forwarded_for,
                       const std::vector<cidr_block>& trusted_proxies,
                       ip_addr& out);

}  // namespace molink
