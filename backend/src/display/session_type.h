#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace molink::display {

/// Which desktop session this process is running under.
///
/// Linux capture and input only have an X11 backend. Inside a Wayland session
/// that backend talks to XWayland, so native Wayland applications are neither
/// visible nor clickable. The type is reported instead of silently degrading.
enum class session_type { x11, wayland, unknown };

inline const char* session_type_name(session_type type) {
  switch (type) {
    case session_type::x11:
      return "x11";
    case session_type::wayland:
      return "wayland";
    case session_type::unknown:
      break;
  }
  return "unknown";
}

namespace detail {

inline bool equals_ascii_case_insensitive(const std::string& value,
                                          const char* expected) {
  std::size_t i = 0;
  for (; i < value.size(); ++i) {
    const char wanted = expected[i];
    if (wanted == '\0') return false;
    const auto actual = static_cast<unsigned char>(value[i]);
    const auto lower = static_cast<unsigned char>(wanted);
    if (std::tolower(actual) != std::tolower(lower)) return false;
  }
  return expected[i] == '\0';
}

}  // namespace detail

/// Classifies a desktop session from the environment variables that separate
/// native X11 from Wayland (and its XWayland compatibility server). Every
/// argument is a raw environment value; an empty string means "not set".
///
/// Priority, highest first:
///   1. XDG_SESSION_TYPE=wayland -> wayland, even when DISPLAY is set (a
///      Wayland session normally exports DISPLAY for XWayland).
///   2. WAYLAND_DISPLAY set and DISPLAY unset -> wayland.
///   3. XDG_SESSION_TYPE=x11, or DISPLAY set with no Wayland display -> x11.
///   4. Otherwise unknown. WAYLAND_DISPLAY and DISPLAY both set without an
///      XDG_SESSION_TYPE is ambiguous (an X11 client inside XWayland looks the
///      same as a Wayland session), so it is not guessed.
///
/// Pure: the runtime reads the three variables and passes them in, which lets
/// every combination be unit-tested without a display server.
inline session_type detect_session_type(const std::string& xdg_session_type,
                                        const std::string& wayland_display,
                                        const std::string& display) {
  if (detail::equals_ascii_case_insensitive(xdg_session_type, "wayland")) {
    return session_type::wayland;
  }
  const bool has_wayland = !wayland_display.empty();
  const bool has_x11 = !display.empty();
  if (has_wayland && !has_x11) return session_type::wayland;
  if (detail::equals_ascii_case_insensitive(xdg_session_type, "x11")) {
    return session_type::x11;
  }
  if (has_x11 && !has_wayland) return session_type::x11;
  return session_type::unknown;
}

}  // namespace molink::display
