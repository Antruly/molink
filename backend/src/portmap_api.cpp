#include "portmap_api.h"

#include <cstring>

namespace molink {
namespace {

constexpr portmap_preset kPresets[] = {
    {"rdp", 3389},   {"ssh", 22},   {"mysql", 3306},
    {"http", 80},    {"https", 443}, {"smb", 445},
};

bool starts_with(const std::string& text, const char* prefix) {
  const std::size_t length = std::strlen(prefix);
  return text.size() >= length && text.compare(0, length, prefix) == 0;
}

}  // namespace

const char* portmap_error_code(portmap_error error) {
  switch (error) {
    case portmap_error::bad_port:
      return "bad_port";
    case portmap_error::bad_target:
      return "bad_target";
    case portmap_error::port_in_use:
      return "port_in_use";
    case portmap_error::not_found:
      return "not_found";
    case portmap_error::port_forward_disabled:
      return "port_forward_disabled";
    case portmap_error::save_failed:
      return "save_failed";
    case portmap_error::none:
      break;
  }
  return "ok";
}

int portmap_error_http_status(portmap_error error) {
  switch (error) {
    case portmap_error::none:
      return 200;
    case portmap_error::not_found:
      return 404;
    case portmap_error::port_in_use:
      return 409;
    case portmap_error::port_forward_disabled:
      return 403;
    case portmap_error::save_failed:
      return 500;
    case portmap_error::bad_port:
    case portmap_error::bad_target:
      break;
  }
  return 400;
}

const char* portmap_error_message(portmap_error error) {
  switch (error) {
    case portmap_error::bad_port:
      return "port must be 1..65535 (0 picks a free listen port)";
    case portmap_error::bad_target:
      return "target host must be a non-empty host name or address";
    case portmap_error::port_in_use:
      return "the listen port is unavailable";
    case portmap_error::not_found:
      return "no such port mapping";
    case portmap_error::port_forward_disabled:
      return "port forwarding is disabled";
    case portmap_error::save_failed:
      return "the mapping could not be saved";
    case portmap_error::none:
      break;
  }
  return "";
}

bool portmap_valid_port(long long value, bool allow_zero) {
  const long long minimum = allow_zero ? 0 : 1;
  return value >= minimum && value <= 65535;
}

bool portmap_valid_host(const std::string& host) {
  if (host.empty() || host.size() > 255) return false;
  for (const unsigned char c : host) {
    if (c < 0x20 || c == 0x7f) return false;
    if (c == ' ' || c == '\t') return false;
  }
  return true;
}

portmap_error portmap_error_from_manager(const std::string& error) {
  if (error.empty()) return portmap_error::none;
  if (error == "mapping_not_found") return portmap_error::not_found;
  if (error == "port_forward_disabled") {
    return portmap_error::port_forward_disabled;
  }
  if (starts_with(error, "cannot bind") ||
      starts_with(error, "cannot resolve")) {
    return portmap_error::port_in_use;
  }
  if (starts_with(error, "cannot open settings") ||
      starts_with(error, "cannot write settings") ||
      starts_with(error, "cannot replace settings") ||
      error == "settings path is empty") {
    return portmap_error::save_failed;
  }
  if (error.find("target port") != std::string::npos ||
      error.find("listen port") != std::string::npos) {
    return portmap_error::bad_port;
  }
  if (error.find("target host") != std::string::npos) {
    return portmap_error::bad_target;
  }
  // The manager validated everything before this point; a remaining failure
  // came from the settings write or from a listener that vanished under us.
  return portmap_error::save_failed;
}

const portmap_preset* portmap_presets(std::size_t& count) {
  count = sizeof(kPresets) / sizeof(kPresets[0]);
  return kPresets;
}

}  // namespace molink
