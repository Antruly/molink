// Desktop session classification, without a display server.
#include <cstdio>
#include <string>

#include "display/session_type.h"

namespace {

using molink::display::detect_session_type;
using molink::display::session_type;
using molink::display::session_type_name;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_type(session_type actual, session_type wanted,
                 const char* description) {
  ++checks;
  if (actual == wanted) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s (got %s, want %s)\n", description,
               session_type_name(actual), session_type_name(wanted));
}

const char* const kWayland = "wayland";
const char* const kX11 = "x11";
const std::string kNoValue;

void test_xdg_wayland_wins() {
  // Wayland sessions export DISPLAY for XWayland; the XDG value is the truth.
  expect_type(detect_session_type(kWayland, "wayland-0", ":0"),
              session_type::wayland,
              "XDG_SESSION_TYPE=wayland wins over DISPLAY");
  expect_type(detect_session_type(kWayland, "", ":0"), session_type::wayland,
              "XDG_SESSION_TYPE=wayland alone is enough");
  expect_type(detect_session_type("Wayland", "wayland-0", ""),
              session_type::wayland,
              "the XDG value is case-insensitive");
}

void test_wayland_display_without_x11() {
  expect_type(detect_session_type(kNoValue, "wayland-0", ""),
              session_type::wayland,
              "WAYLAND_DISPLAY without DISPLAY is Wayland");
}

void test_x11_sessions() {
  expect_type(detect_session_type(kX11, "", ":0"), session_type::x11,
              "XDG_SESSION_TYPE=x11");
  expect_type(detect_session_type(kNoValue, "", ":0"), session_type::x11,
              "DISPLAY alone is X11");
  expect_type(detect_session_type("X11", "", ":1"), session_type::x11,
              "the XDG x11 value is case-insensitive");
  // XDG is authoritative even if a stale Wayland variable is also present.
  expect_type(detect_session_type(kX11, "wayland-0", ":0"), session_type::x11,
              "XDG_SESSION_TYPE=x11 wins over WAYLAND_DISPLAY");
}

void test_unknown_and_conflicts() {
  expect_type(detect_session_type(kNoValue, "", ""), session_type::unknown,
              "no session variables is unknown");
  expect_type(detect_session_type("tty", "", ""), session_type::unknown,
              "a non-graphical XDG value is unknown");
  // Both displays set, no XDG: X11-in-XWayland and Wayland look identical.
  expect_type(detect_session_type(kNoValue, "wayland-0", ":0"),
              session_type::unknown,
              "Wayland and X11 displays without XDG is ambiguous");
}

void test_names() {
  expect(std::string(session_type_name(session_type::x11)) == "x11",
         "x11 name");
  expect(std::string(session_type_name(session_type::wayland)) == "wayland",
         "wayland name");
  expect(std::string(session_type_name(session_type::unknown)) == "unknown",
         "unknown name");
}

}  // namespace

int main() {
  test_xdg_wayland_wins();
  test_wayland_display_without_x11();
  test_x11_sessions();
  test_unknown_and_conflicts();
  test_names();

  if (failures != 0) {
    std::fprintf(stderr, "%d/%d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("%d checks passed\n", checks);
  return 0;
}
