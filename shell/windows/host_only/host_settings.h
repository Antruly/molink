// Pure settings helpers for the host-only shell.
//
// The core's /api/settings is a POST with query parameters, not a JSON PUT.
// These functions build those queries and mirror the backend's validation
// rules so the UI can reject a bad value before the core does.
#pragma once

#include <string>

namespace molink {
namespace hostui {

/// The two independent switches the minimal UI exposes. The backend models
/// them as one preset:
///   input on  + clipboard on  -> "full"
///   input on  + clipboard off -> "assist"
///   input off                 -> "view_only" (clipboard cannot be on alone)
struct access_switches {
  bool input = true;
  bool clipboard = true;
};

/// Trim surrounding spaces and accept 1..65535. Returns false for empty,
/// non-numeric or out-of-range input.
bool parse_listen_port(const std::string& text, int& port);

std::string access_policy_from_switches(bool input_allowed,
                                        bool clipboard_allowed);
access_switches switches_from_access_policy(const std::string& policy);

/// Mirrors auth_state.cpp::valid_long_term: 8..128 chars, no whitespace or
/// control characters, and at least one letter plus one digit. @p error is a
/// Chinese message suitable for the status line.
bool valid_long_term_code(const std::string& value, std::string& error);

/// Percent-encode everything outside the RFC 3986 unreserved set.
std::string url_encode(const std::string& value);

/// "allow_remote=1&discovery=0&host_only=1&access_policy=full"
std::string settings_query(bool allow_remote, bool discovery, bool host_only,
                           const std::string& access_policy);

/// "tcp=28418&udp=28422". The core's handler requires both values.
std::string network_query(int tcp_port, int udp_port);

/// "mode=multi&uses=20&value=123456". A non-empty current code is kept.
std::string auth_code_query(const std::string& mode, int uses,
                            const std::string& current_code);

/// "value=<encoded>".
std::string long_term_query(const std::string& value);

}  // namespace hostui
}  // namespace molink
