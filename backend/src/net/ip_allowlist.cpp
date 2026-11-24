#include "net/ip_allowlist.h"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace molink {
namespace {

struct parsed_address {
  allowlist_entry::family kind = allowlist_entry::family::v4;
  std::uint8_t bytes[16] = {};
};

bool parse_address(const std::string& text, parsed_address& out) {
  if (text.empty()) return false;

  in_addr v4{};
  if (::inet_pton(AF_INET, text.c_str(), &v4) == 1) {
    out.kind = allowlist_entry::family::v4;
    std::memset(out.bytes, 0, sizeof(out.bytes));
    std::memcpy(out.bytes, &v4, 4);
    return true;
  }

  in6_addr v6{};
  if (::inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
    out.kind = allowlist_entry::family::v6;
    std::memcpy(out.bytes, &v6, sizeof(out.bytes));
    return true;
  }
  return false;
}

/// An IPv4-mapped IPv6 address is ::ffff:a.b.c.d.
bool is_v4_mapped(const std::uint8_t bytes[16]) {
  for (int i = 0; i < 10; ++i) {
    if (bytes[i] != 0) return false;
  }
  return bytes[10] == 0xff && bytes[11] == 0xff;
}

/// Wraps an IPv4 address as ::ffff:a.b.c.d.
void to_v4_mapped(const std::uint8_t v4[4], std::uint8_t out[16]) {
  std::memset(out, 0, 16);
  out[10] = 0xff;
  out[11] = 0xff;
  std::memcpy(out + 12, v4, 4);
}

bool prefix_matches(const std::uint8_t* a, const std::uint8_t* b, int bits) {
  const int whole = bits / 8;
  if (whole > 0 && std::memcmp(a, b, static_cast<std::size_t>(whole)) != 0) {
    return false;
  }
  const int rest = bits % 8;
  if (rest == 0) return true;
  const auto mask = static_cast<std::uint8_t>(0xffu << (8 - rest));
  return (a[whole] & mask) == (b[whole] & mask);
}

int max_prefix(allowlist_entry::family kind) {
  return kind == allowlist_entry::family::v4 ? 32 : 128;
}

/// Parses the prefix-length digits. Returns false for anything that is not a
/// non-empty run of decimal digits or that exceeds @p max.
bool parse_prefix(const std::string& text, int max, int& out) {
  if (text.empty()) return false;
  int value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') return false;
    value = value * 10 + (ch - '0');
    // A prefix is at most 128; stop before the int can overflow on absurd
    // input like a hundred digits.
    if (value > 128) return false;
  }
  if (value > max) return false;
  out = value;
  return true;
}

bool fail(allowlist_error* error, std::size_t entry,
          const std::string& reason) {
  if (error != nullptr) {
    error->entry = entry;
    error->message = "entry " + std::to_string(entry) + ": " + reason;
  }
  return false;
}

}  // namespace

bool allowlist_is_loopback(const std::string& peer_ip) {
  if (peer_ip == "localhost") return true;

  parsed_address address;
  if (!parse_address(peer_ip, address)) return false;
  if (address.kind == allowlist_entry::family::v4) {
    return address.bytes[0] == 127;
  }
  // ::1
  static const std::uint8_t kLoopbackV6[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                               0, 0, 0, 0, 0, 0, 0, 1};
  if (std::memcmp(address.bytes, kLoopbackV6, 16) == 0) return true;
  // ::ffff:127.0.0.0/104
  return is_v4_mapped(address.bytes) && address.bytes[12] == 127;
}

bool allowlist_parse(const std::string& text, allowlist& out,
                     allowlist_error* error) {
  out.entries.clear();
  if (error != nullptr) *error = allowlist_error{};

  // Split on ASCII whitespace. Empty runs collapse, so trailing newlines from
  // a textarea do not count as an empty entry.
  std::vector<std::string> tokens;
  std::string current;
  for (const char ch : text) {
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) tokens.push_back(current);

  std::vector<allowlist_entry> parsed;
  parsed.reserve(tokens.size());
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const std::size_t entry_number = i + 1;
    const std::string& token = tokens[i];
    const std::size_t slash = token.find('/');
    if (slash == std::string::npos) {
      return fail(error, entry_number,
                  "\"" + token + "\" is missing a prefix length (write "
                  "address/prefix)");
    }
    if (token.find('/', slash + 1) != std::string::npos) {
      return fail(error, entry_number,
                  "\"" + token + "\" has more than one '/'");
    }

    const std::string address_text = token.substr(0, slash);
    const std::string prefix_text = token.substr(slash + 1);
    if (address_text.empty()) {
      return fail(error, entry_number, "\"" + token + "\" has no address");
    }
    if (prefix_text.empty()) {
      return fail(error, entry_number,
                  "\"" + token + "\" has an empty prefix length");
    }

    parsed_address address;
    if (!parse_address(address_text, address)) {
      return fail(error, entry_number,
                  "\"" + address_text + "\" is not a valid IPv4 or IPv6 "
                  "address");
    }

    int prefix = 0;
    if (!parse_prefix(prefix_text, max_prefix(address.kind), prefix)) {
      return fail(error, entry_number,
                  "\"" + token + "\" has an invalid prefix length (IPv" +
                      std::string(address.kind == allowlist_entry::family::v4
                                      ? "4 must be 0..32"
                                      : "6 must be 0..128") +
                      ")");
    }

    allowlist_entry entry;
    entry.kind = address.kind;
    std::memcpy(entry.bytes, address.bytes, sizeof(entry.bytes));
    entry.prefix = prefix;
    parsed.push_back(entry);
  }

  out.entries = std::move(parsed);
  return true;
}

bool allowlist_allows(const allowlist& list, const std::string& peer_ip) {
  if (list.entries.empty()) return true;
  if (allowlist_is_loopback(peer_ip)) return true;

  parsed_address peer;
  if (!parse_address(peer_ip, peer)) {
    // Not an address we can attribute (for example the "relay" label used by
    // the relay transports). The allowlist does not apply; authentication
    // still does.
    return true;
  }

  // Check both spellings of a v4-mapped peer so a 192.168.2.0/24 rule and an
  // ::ffff:192.168.2.0/120 rule both work whichever way the socket reports
  // the address.
  parsed_address candidates[2];
  int candidate_count = 1;
  candidates[0] = peer;
  bool second_is_synthetic_mapped = false;
  if (peer.kind == allowlist_entry::family::v4) {
    candidates[1].kind = allowlist_entry::family::v6;
    to_v4_mapped(peer.bytes, candidates[1].bytes);
    candidate_count = 2;
    second_is_synthetic_mapped = true;
  } else if (is_v4_mapped(peer.bytes)) {
    candidates[1].kind = allowlist_entry::family::v4;
    candidates[1].bytes[0] = peer.bytes[12];
    candidates[1].bytes[1] = peer.bytes[13];
    candidates[1].bytes[2] = peer.bytes[14];
    candidates[1].bytes[3] = peer.bytes[15];
    candidate_count = 2;
  }

  for (const allowlist_entry& entry : list.entries) {
    for (int i = 0; i < candidate_count; ++i) {
      // A plain IPv4 peer only matches the synthetic ::ffff:a.b.c.d form
      // when the rule actually covers that mapping (/96 or longer). Without
      // this, ::/0 would silently include every IPv4 address.
      if (second_is_synthetic_mapped && i == 1 && entry.prefix < 96) continue;
      if (entry.kind != candidates[i].kind) continue;
      if (prefix_matches(entry.bytes, candidates[i].bytes, entry.prefix)) {
        return true;
      }
    }
  }
  return false;
}

bool allowlist_allows_text(const std::string& text, const std::string& peer_ip) {
  // Loopback bypasses everything, including a hand-edited list that no longer
  // parses, so the local UI can always repair it.
  if (allowlist_is_loopback(peer_ip)) return true;

  allowlist list;
  if (!allowlist_parse(text, list)) {
    // The settings API rejects invalid lists, so this is a hand-edited
    // config. Failing closed keeps a typo from silently opening the host.
    return false;
  }
  return allowlist_allows(list, peer_ip);
}

}  // namespace molink
