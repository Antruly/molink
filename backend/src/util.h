#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace molink {

/// Bind addresses that only accept local connections. The core listens on all
/// interfaces by default (0.0.0.0); pass a loopback host to keep it local.
/// Exposed here so the ready line and the HTTP handlers describe the same thing.
bool is_loopback_host(const std::string& host);

/// Peer addresses that count as the local machine. Covers IPv4, IPv6 and the
/// IPv4-mapped IPv6 form libuv reports on dual-stack sockets.
bool is_loopback_peer(const std::string& peer);

/// The OS session this process runs in. A core started by a service lives in
/// session 0 and cannot capture an interactive desktop, so a shell must not
/// reuse it: the session id is compared before an existing core is adopted.
uint32_t current_session_id();

/// Host part for a URL: IPv6 literals need brackets.
std::string format_url_host(const std::string& host);

/// Uppercase base32-ish alphabet without I/L/O/0/1, so a session id can be read
/// aloud or typed from a screenshot without ambiguity.
std::string make_session_public_id();

/// Decode UTF-8 into Unicode scalar values. Invalid sequences are skipped.
std::vector<uint32_t> utf8_codepoints(const std::string& text);

/// Monotonic milliseconds since an arbitrary origin.
uint64_t now_ms();

/// Standard base64 (with padding). Used for small binary payloads embedded in
/// JSON control messages, such as the cursor bitmap.
std::string base64_encode(const uint8_t* data, std::size_t len);

/// Percent-encode a value for a query string (`unreserved` characters stay).
std::string url_encode(const std::string& value);

/// Non-loopback IPv4 addresses of this machine, in interface order.
std::vector<std::string> local_ipv4_addresses();

/// Convert a UTF-8 string from the wire or JSON into a filesystem path.
///
/// On Windows the implicit `std::filesystem::path(std::string)` conversion
/// interprets the bytes in the ANSI code page, so a zh-CN host (CP_ACP 936)
/// turns a UTF-8 "照片" into garbage. Every path that crosses the network must
/// go through these helpers instead. Header-only so tests that compile one
/// source file do not have to drag in the rest of util.cpp.
inline std::filesystem::path path_from_utf8(const std::string& text) {
#if defined(__cpp_lib_char8_t)
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
  // The only spelling available to the C++17 Win7 kit.
  return std::filesystem::u8path(text);
#endif
}

/// The inverse of `path_from_utf8`: native path -> UTF-8 for JSON and logs.
inline std::string path_to_utf8(const std::filesystem::path& path) {
#if defined(__cpp_lib_char8_t)
  const std::u8string value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
#else
  return path.u8string();
#endif
}

}  // namespace molink
