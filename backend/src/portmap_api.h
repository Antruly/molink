#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace molink {

/// Uniform failure codes for `/api/portmap`. The handler turns these into the
/// documented `code` strings and HTTP statuses; keeping the mapping here (and
/// free of libuvcpp) lets the core tests cover it without an HTTP server.
enum class portmap_error {
  none,
  bad_port,
  bad_target,
  port_in_use,
  not_found,
  port_forward_disabled,
  save_failed,
};

const char* portmap_error_code(portmap_error error);
int portmap_error_http_status(portmap_error error);
/// Human-readable text for logs and the API's `message` field. The UI shows
/// its own localized text from the stable `code`.
const char* portmap_error_message(portmap_error error);

/// 1..65535, or 0 as well when @p allow_zero (listen port 0 = system pick).
bool portmap_valid_port(long long value, bool allow_zero);
/// A non-empty host name / address that can be stored in the settings line.
bool portmap_valid_host(const std::string& host);

/// Maps a `port_forward_manager` error string onto the API vocabulary.
/// Unknown write failures become `save_failed`; unknown bind failures become
/// `port_in_use` because that is the only listen-side failure users can fix.
portmap_error portmap_error_from_manager(const std::string& error);

struct portmap_preset {
  const char* id;
  uint16_t port;
};

/// The six shortcuts offered in the UI (RDP, SSH, MySQL, HTTP, HTTPS, SMB).
const portmap_preset* portmap_presets(std::size_t& count);

}  // namespace molink
