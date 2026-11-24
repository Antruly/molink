#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace molink {

/// One CIDR entry. IPv4 addresses are kept in the first four bytes; IPv6
/// addresses use all sixteen. Host bits are preserved as supplied; matching
/// only compares the prefix, so 192.168.1.7/24 behaves like 192.168.1.0/24.
struct allowlist_entry {
  enum class family { v4, v6 };

  family kind = family::v4;
  std::uint8_t bytes[16] = {};
  int prefix = 0;
};

struct allowlist {
  std::vector<allowlist_entry> entries;
};

/// Why a list was rejected. @c entry is the 1-based position of the offending
/// token in the input, so the settings API can point at it.
struct allowlist_error {
  std::size_t entry = 0;
  std::string message;
};

/// Parses whitespace-separated CIDR entries (spaces, tabs, CR or LF between
/// them). Every entry must be `address/prefix` with a prefix of 0..32 for
/// IPv4 or 0..128 for IPv6. The whole list is rejected if any entry is
/// invalid; on failure @p out is left empty. An empty or whitespace-only
/// input is a valid empty list. @p error may be null.
bool allowlist_parse(const std::string& text, allowlist& out,
                     allowlist_error* error = nullptr);

/// True when @p peer_ip may connect:
///   * an empty list imposes no restriction;
///   * loopback is always allowed (127.0.0.0/8, ::1 and their IPv4-mapped
///     spellings), so the local UI cannot lock itself out;
///   * otherwise the peer must fall inside one of the entries.
///
/// IPv4 peers are also matched against IPv4-mapped IPv6 entries and vice
/// versa. A peer that is not an IP literal (the relay transports report
/// "relay") cannot be attributed to a source address; it is not filtered
/// here and still has to pass normal authentication.
bool allowlist_allows(const allowlist& list, const std::string& peer_ip);

/// Convenience wrapper used by the request paths. An unparsable list fails
/// closed for non-loopback peers; loopback is still allowed.
bool allowlist_allows_text(const std::string& text, const std::string& peer_ip);

/// True for 127.0.0.0/8, ::1 and their IPv4-mapped forms.
bool allowlist_is_loopback(const std::string& peer_ip);

}  // namespace molink
