// CIDR parsing and matching for the IP allowlist. Pure libc (inet_pton), no
// config directory, no libuvcpp.
#include <cstdint>
#include <cstdio>
#include <string>

#include "net/ip_allowlist.h"

namespace {

using molink::allowlist;
using molink::allowlist_allows;
using molink::allowlist_allows_text;
using molink::allowlist_entry;
using molink::allowlist_error;
using molink::allowlist_is_loopback;
using molink::allowlist_parse;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

allowlist parse_ok(const std::string& text, const char* description) {
  allowlist list;
  allowlist_error error;
  const bool ok = allowlist_parse(text, list, &error);
  expect(ok, description);
  if (!ok) {
    std::fprintf(stderr, "  parse error: entry %zu: %s\n", error.entry,
                 error.message.c_str());
  }
  return list;
}

void expect_parse_error(const std::string& text, std::size_t entry,
                        const char* description) {
  allowlist list;
  allowlist_error error;
  const bool ok = allowlist_parse(text, list, &error);
  expect(!ok, description);
  expect(entry == error.entry, "error names the offending entry");
  expect(list.entries.empty(), "failed parse clears the output list");
  expect(!error.message.empty(), "error message is not empty");
}

void expect_v4(const allowlist_entry& entry, int a, int b, int c, int d,
               int prefix) {
  expect(entry.kind == allowlist_entry::family::v4, "entry is IPv4");
  expect(entry.prefix == prefix, "IPv4 prefix length");
  expect(entry.bytes[0] == a && entry.bytes[1] == b && entry.bytes[2] == c &&
             entry.bytes[3] == d,
         "IPv4 address bytes");
}

void test_parse_v4() {
  const allowlist empty = parse_ok("", "empty list is valid");
  expect(empty.entries.empty(), "empty list has no entries");
  const allowlist blank = parse_ok(" \t\r\n  ", "whitespace-only list is valid");
  expect(blank.entries.empty(), "whitespace-only list has no entries");

  const allowlist single = parse_ok("192.168.2.0/24", "IPv4 /24 parses");
  expect(single.entries.size() == 1, "one IPv4 entry");
  if (!single.entries.empty()) expect_v4(single.entries[0], 192, 168, 2, 0, 24);

  const allowlist host = parse_ok("10.0.0.5/32", "IPv4 /32 parses");
  if (!host.entries.empty()) expect_v4(host.entries[0], 10, 0, 0, 5, 32);

  const allowlist all = parse_ok("0.0.0.0/0", "IPv4 /0 parses");
  if (!all.entries.empty()) expect_v4(all.entries[0], 0, 0, 0, 0, 0);

  // Host bits are kept but ignored by matching; the parser accepts them.
  const allowlist host_bits = parse_ok("192.168.1.77/24", "host bits accepted");
  if (!host_bits.entries.empty()) {
    expect_v4(host_bits.entries[0], 192, 168, 1, 77, 24);
  }
}

void test_parse_v6() {
  const allowlist prefix = parse_ok("fd00::/8", "IPv6 /8 parses");
  expect(prefix.entries.size() == 1, "one IPv6 entry");
  if (!prefix.entries.empty()) {
    expect(prefix.entries[0].kind == allowlist_entry::family::v6,
           "entry is IPv6");
    expect(prefix.entries[0].prefix == 8, "IPv6 /8 prefix");
    expect(prefix.entries[0].bytes[0] == 0xfd, "IPv6 first byte");
  }

  const allowlist host = parse_ok("fd00::1/128", "IPv6 /128 parses");
  if (!host.entries.empty()) {
    expect(host.entries[0].prefix == 128, "IPv6 /128 prefix");
    expect(host.entries[0].bytes[15] == 1, "IPv6 last byte");
  }

  const allowlist all = parse_ok("::/0", "IPv6 /0 parses");
  if (!all.entries.empty()) {
    expect(all.entries[0].prefix == 0, "IPv6 /0 prefix");
  }

  const allowlist compressed = parse_ok("fd00:1234:5678:9abc::/64",
                                        "compressed IPv6 /64 parses");
  if (!compressed.entries.empty()) {
    expect(compressed.entries[0].prefix == 64, "compressed IPv6 prefix");
  }
}

void test_parse_list() {
  const allowlist spaced = parse_ok(
      "192.168.2.0/24 10.0.0.5/32 fd00::/8", "space-separated list parses");
  expect(spaced.entries.size() == 3, "three entries");
  if (spaced.entries.size() == 3) {
    expect_v4(spaced.entries[0], 192, 168, 2, 0, 24);
    expect_v4(spaced.entries[1], 10, 0, 0, 5, 32);
    expect(spaced.entries[2].kind == allowlist_entry::family::v6,
           "third entry is IPv6");
  }

  const allowlist newlines = parse_ok(
      "192.168.2.0/24\r\n10.0.0.5/32\nfd00::/8", "newlines are separators");
  expect(newlines.entries.size() == 3, "three newline-separated entries");
  const allowlist tabs = parse_ok("192.168.2.0/24\t10.0.0.5/32",
                                  "tabs are separators");
  expect(tabs.entries.size() == 2, "two tab-separated entries");
  const allowlist mixed = parse_ok(" 192.168.2.0/24 \n\t 10.0.0.5/32 ",
                                   "surrounding whitespace is ignored");
  expect(mixed.entries.size() == 2, "two entries with surrounding whitespace");
}

void test_parse_errors() {
  expect_parse_error("192.168.1.1/33", 1, "IPv4 prefix above 32 rejected");
  expect_parse_error("192.168.1.1/-1", 1, "negative prefix rejected");
  expect_parse_error("192.168.1.1/24x", 1, "non-numeric prefix rejected");
  expect_parse_error("fd00::/129", 1, "IPv6 prefix above 128 rejected");
  expect_parse_error("fd00::/x", 1, "non-numeric IPv6 prefix rejected");
  expect_parse_error("abc", 1, "bare word rejected");
  expect_parse_error("/24", 1, "empty address rejected");
  expect_parse_error("192.168.1.1/", 1, "empty prefix rejected");
  expect_parse_error("192.168.1.1", 1, "missing prefix rejected");
  expect_parse_error("192.168.1.1/24/8", 1, "two slashes rejected");
  expect_parse_error("300.1.1.1/24", 1, "invalid IPv4 octet rejected");
  expect_parse_error("192.168.1.1/24 abc", 2,
                     "invalid second entry rejected and index reported");
  expect_parse_error("fd00::/8 192.168.1.1/33", 2,
                     "invalid second prefix rejected");

  // "The whole list is rejected": no partial entries survive.
  allowlist list;
  allowlist_error error;
  expect(!allowlist_parse("10.1.0.0/16 192.168.1.1/33", list, &error),
         "list with one bad entry fails");
  expect(list.entries.empty(), "no entries from a rejected list");
  expect(error.entry == 2, "second entry named");

  // A null error pointer is allowed.
  allowlist ignored;
  expect(!allowlist_parse("nonsense", ignored, nullptr),
         "parse works without an error sink");
  expect(ignored.entries.empty(), "no entries after a null-sink failure");
}

void test_empty_list_allows() {
  const allowlist empty;
  expect(allowlist_allows(empty, "192.168.2.20"), "empty list allows IPv4");
  expect(allowlist_allows(empty, "fd00::1"), "empty list allows IPv6");
  expect(allowlist_allows(empty, "8.8.8.8"), "empty list allows the internet");
  expect(allowlist_allows(empty, "not-an-ip"), "empty list allows anything");
  expect(allowlist_allows_text("", "203.0.113.9"),
         "empty text allows any peer");
}

void test_loopback_always_allowed() {
  const allowlist only_lan = parse_ok("192.168.2.0/24", "test list parses");
  expect(allowlist_allows(only_lan, "127.0.0.1"), "127.0.0.1 allowed");
  expect(allowlist_allows(only_lan, "127.255.255.254"), "127/8 allowed");
  expect(allowlist_allows(only_lan, "::1"), "::1 allowed");
  expect(allowlist_allows(only_lan, "::ffff:127.0.0.1"),
         "IPv4-mapped ::1 allowed");
  expect(allowlist_allows_text("192.168.2.0/24", "127.0.0.1"),
         "text wrapper allows loopback");
  expect(allowlist_is_loopback("127.0.0.1"), "loopback predicate 127.0.0.1");
  expect(allowlist_is_loopback("127.9.8.7"), "loopback predicate 127/8");
  expect(allowlist_is_loopback("::1"), "loopback predicate ::1");
  expect(!allowlist_is_loopback("192.168.2.20"),
         "LAN address is not loopback");
  expect(!allowlist_is_loopback("::2"), "::2 is not loopback");
}

void test_v4_boundaries() {
  allowlist list;
  expect(allowlist_parse("192.168.1.0/24", list, nullptr), "/24 parses");
  expect(allowlist_allows(list, "192.168.1.0"), "network address allowed");
  expect(allowlist_allows(list, "192.168.1.255"), "broadcast address allowed");
  expect(allowlist_allows(list, "192.168.1.128"), "middle address allowed");
  expect(!allowlist_allows(list, "192.168.0.255"), "address below denied");
  expect(!allowlist_allows(list, "192.168.2.0"), "next subnet denied");
  expect(!allowlist_allows(list, "10.0.0.1"), "unrelated address denied");

  allowlist host;
  expect(allowlist_parse("10.0.0.5/32", host, nullptr), "/32 parses");
  expect(allowlist_allows(host, "10.0.0.5"), "/32 exact address allowed");
  expect(!allowlist_allows(host, "10.0.0.4"), "/32 neighbour denied");
  expect(!allowlist_allows(host, "10.0.0.6"), "/32 next neighbour denied");

  allowlist all;
  expect(allowlist_parse("0.0.0.0/0", all, nullptr), "v4 /0 parses");
  expect(allowlist_allows(all, "8.8.8.8"), "v4 /0 allows any IPv4");
  expect(allowlist_allows(all, "203.0.113.7"), "v4 /0 allows documentation IP");
  expect(!allowlist_allows(all, "fd00::1"), "v4 /0 does not allow IPv6");
}

void test_cross_prefix() {
  allowlist list;
  expect(allowlist_parse("192.168.2.0/23", list, nullptr), "/23 parses");
  expect(allowlist_allows(list, "192.168.2.255"), "/23 first half allowed");
  expect(allowlist_allows(list, "192.168.3.0"), "/23 second half allowed");
  expect(!allowlist_allows(list, "192.168.1.255"), "/23 address before denied");
  expect(!allowlist_allows(list, "192.168.4.1"), "/23 address after denied");
}

void test_v6_matching() {
  allowlist list;
  expect(allowlist_parse("fd00::/8", list, nullptr), "v6 /8 parses");
  expect(allowlist_allows(list, "fd00::1"), "compressed v6 allowed");
  expect(allowlist_allows(list, "fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"),
         "v6 /8 upper bound allowed");
  expect(!allowlist_allows(list, "fe80::1"), "v6 outside /8 denied");

  allowlist subnet;
  expect(allowlist_parse("fd00:1234:5678:9abc::/64", subnet, nullptr),
         "v6 /64 parses");
  expect(allowlist_allows(subnet, "fd00:1234:5678:9abc::1"),
         "v6 /64 address allowed");
  expect(allowlist_allows(subnet, "fd00:1234:5678:9abc:ffff::1"),
         "v6 /64 upper half allowed");
  expect(!allowlist_allows(subnet, "fd00:1234:5678:9abd::1"),
         "v6 /64 next subnet denied");

  allowlist host;
  expect(allowlist_parse("fd00::1/128", host, nullptr), "v6 /128 parses");
  expect(allowlist_allows(host, "fd00::1"), "v6 /128 exact allowed");
  expect(!allowlist_allows(host, "fd00::2"), "v6 /128 neighbour denied");

  allowlist all;
  expect(allowlist_parse("::/0", all, nullptr), "v6 /0 parses");
  expect(allowlist_allows(all, "fd00::1"), "v6 /0 allows any IPv6");
  expect(!allowlist_allows(all, "192.168.2.20"),
         "v6 /0 does not allow a plain IPv4 peer");

  allowlist mapped_all;
  expect(allowlist_parse("::ffff:0:0/96", mapped_all, nullptr),
         "mapped-all rule parses");
  expect(allowlist_allows(mapped_all, "192.168.2.20"),
         "::ffff:0:0/96 allows a plain IPv4 peer");
}

void test_v4_mapped() {
  allowlist v4_rule;
  expect(allowlist_parse("192.168.2.0/24", v4_rule, nullptr), "v4 rule parses");
  expect(allowlist_allows(v4_rule, "::ffff:192.168.2.20"),
         "v4 rule matches a mapped peer");
  expect(!allowlist_allows(v4_rule, "::ffff:192.168.3.20"),
         "v4 rule rejects a mapped peer outside it");

  allowlist mapped_rule;
  expect(allowlist_parse("::ffff:192.168.2.0/120", mapped_rule, nullptr),
         "mapped v6 rule parses");
  expect(allowlist_allows(mapped_rule, "192.168.2.20"),
         "mapped v6 rule matches a plain v4 peer");
  expect(!allowlist_allows(mapped_rule, "192.168.3.20"),
         "mapped v6 rule rejects a plain v4 peer outside it");

  expect(!allowlist_is_loopback("::ffff:192.168.2.20"),
         "mapped LAN address is not loopback");
}

void test_multiple_entries_and_unattributable() {
  allowlist list;
  expect(allowlist_parse("10.0.0.0/8 fd00::/8 192.168.2.0/24", list, nullptr),
         "multi-entry list parses");
  expect(allowlist_allows(list, "10.9.8.7"), "first entry matches");
  expect(allowlist_allows(list, "fd00::7"), "second entry matches");
  expect(allowlist_allows(list, "192.168.2.20"), "third entry matches");
  expect(!allowlist_allows(list, "172.16.0.1"), "unlisted address denied");

  // Relay transports have no source address to attribute. They are not
  // filtered here and still need authentication.
  expect(allowlist_allows(list, "relay"), "relay label is not filtered");
  expect(allowlist_allows(list, "not-an-ip"),
         "unattributable peer is not filtered");

  // A malformed list (hand-edited config) fails closed, except loopback.
  expect(!allowlist_allows_text("192.168.1.1/33", "192.168.2.20"),
         "invalid list fails closed for a non-loopback peer");
  expect(allowlist_allows_text("192.168.1.1/33", "127.0.0.1"),
         "invalid list still allows loopback");
}

}  // namespace

int main() {
  test_parse_v4();
  test_parse_v6();
  test_parse_list();
  test_parse_errors();
  test_empty_list_allows();
  test_loopback_always_allowed();
  test_v4_boundaries();
  test_cross_prefix();
  test_v6_matching();
  test_v4_mapped();
  test_multiple_entries_and_unattributable();
  if (failures == 0) {
    std::printf("ip_allowlist: %d checks passed\n", checks);
    return 0;
  }
  std::fprintf(stderr, "ip_allowlist: %d/%d checks failed\n", failures, checks);
  return 1;
}
