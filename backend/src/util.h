#pragma once

#include <cstdint>
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

}  // namespace molink
