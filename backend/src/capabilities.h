#pragma once

#include <cstdint>
#include <string>

namespace molink {

/// What one authenticated session may do. The old behaviour is "everything";
/// a host can pick a narrower preset, and an access request can be approved
/// with a preset just for that session. The bits are stable on the wire as
/// booleans in the welcome, so an old client simply ignores them.
enum class capability : uint32_t {
  screen = 1u << 0,
  input = 1u << 1,
  clipboard_read = 1u << 2,
  clipboard_write = 1u << 3,
  files_read = 1u << 4,
  files_write = 1u << 5,
  audio = 1u << 6,
  chat = 1u << 7,
  logs = 1u << 8,
  restart = 1u << 9,
};

constexpr uint32_t capability_bit(capability value) {
  return static_cast<uint32_t>(value);
}

constexpr uint32_t kCapabilityAll =
    (1u << 10) - 1u;

inline bool has_capability(uint32_t mask, capability value) {
  return (mask & capability_bit(value)) != 0;
}

inline bool valid_access_policy(const std::string& policy) {
  return policy == "full" || policy == "view_only" || policy == "assist";
}

/// Default capabilities for a host-wide policy. "view_only" keeps the picture
/// and the host's audio; "assist" keeps the picture and keyboard/mouse only.
inline uint32_t capabilities_for_policy(const std::string& policy) {
  if (policy == "view_only") {
    return capability_bit(capability::screen) |
           capability_bit(capability::audio);
  }
  if (policy == "assist") {
    return capability_bit(capability::screen) |
           capability_bit(capability::input);
  }
  return kCapabilityAll;
}

inline const char* capability_name(capability value) {
  switch (value) {
    case capability::screen:
      return "screen";
    case capability::input:
      return "input";
    case capability::clipboard_read:
      return "clipboard_read";
    case capability::clipboard_write:
      return "clipboard_write";
    case capability::files_read:
      return "files_read";
    case capability::files_write:
      return "files_write";
    case capability::audio:
      return "audio";
    case capability::chat:
      return "chat";
    case capability::logs:
      return "logs";
    case capability::restart:
      return "restart";
  }
  return "unknown";
}

}  // namespace molink
