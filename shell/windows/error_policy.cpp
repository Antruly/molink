#include "error_policy.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace molink::native::alerts {
namespace {

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

bool starts_with(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

/// A staged/uploaded clipboard file is a confirmation, not a failure.
bool info_code(const std::string& code) {
  static const std::set<std::string> codes = {
      "clipboard_files_staged", "clipboard_files_uploaded",
      "clipboard_files_compat",
  };
  return codes.count(code) != 0;
}

/// These were refused or cut short, but the session and its data are intact.
bool warn_code(const std::string& code) {
  static const std::set<std::string> codes = {
      "bad_input",       "unknown_input",      "unknown_key",
      "bad_key",         "bad_clipboard",      "bad_file_action",
      "bad_action",      "unknown_action",     "file_error",
      "display_mode_busy", "clipboard_too_large",
  };
  if (codes.count(code) != 0) return true;
  // The Web groups input, clipboard and file failures by channel prefix.
  return starts_with(code, "clipboard_") ||
         starts_with(code, "remote_clipboard_") ||
         starts_with(code, "input_") || starts_with(code, "file_");
}

}  // namespace

bool is_session_error(const std::string& code) {
  // Transport closed.
  if (code == "transport_closed" || code == "connection_lost" ||
      code == "connection_closed" || code == "closed" ||
      code == "ws_close" || code == "socket_closed") {
    return true;
  }
  // Credential failures.
  if (code == "unauthorized" || code == "invalid_token" ||
      code == "invalid_code" || code == "too_many_attempts" ||
      code == "ip_banned") {
    return true;
  }
  // Handshake failures.
  if (code == "handshake_failed" || code == "handshake_timeout" ||
      code == "hello_failed" || code == "hello_timeout" ||
      code == "hello_rejected") {
    return true;
  }
  return false;
}

bool looks_like_session_failure(const std::string& message) {
  const std::string text = lower(message);
  static const char* const needles[] = {
      "hello timeout", "hello failed", "hello rejected",
      "handshake failed", "handshake timeout",
      "invalid token", "unauthorized", "too many attempts",
      "ip banned", "connection lost", "connection closed",
      "connection failed", "transport closed", "closed (1008)",
      "could not reach the device", "relay is unavailable",
  };
  for (const char* needle : needles) {
    if (text.find(needle) != std::string::npos) return true;
  }
  return false;
}

route classify(const std::string& code, const std::string& message) {
  route result;
  if (is_session_error(code) ||
      (code.empty() && looks_like_session_failure(message))) {
    result.where = surface::banner;
    result.level = tone::error;
    result.reconnect = true;
    return result;
  }
  result.where = surface::toast;
  if (info_code(code)) {
    result.level = tone::info;
  } else if (warn_code(code)) {
    result.level = tone::warn;
  } else {
    result.level = tone::error;
  }
  return result;
}

const char* tone_name(tone value) {
  switch (value) {
    case tone::info:
      return "info";
    case tone::warn:
      return "warn";
    case tone::error:
    default:
      return "error";
  }
}

}  // namespace molink::native::alerts
