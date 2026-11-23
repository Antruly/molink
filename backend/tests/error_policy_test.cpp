// Error routing: only a closed transport, the four credential failures and a
// failed handshake reach the session banner. Everything else is a toast, and
// its tone comes from the error code.
#include <cstdio>
#include <string>

#include "error_policy.h"

namespace {

int failures = 0;
int checks = 0;

using molink::native::alerts::classify;
using molink::native::alerts::surface;
using molink::native::alerts::tone;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_toast(const std::string& code, const std::string& message,
                  tone level, const char* description) {
  const auto route = classify(code, message);
  expect(route.where == surface::toast, description);
  expect(route.level == level, description);
  expect(!route.reconnect, description);
}

void test_session_banner() {
  const char* codes[] = {"transport_closed", "unauthorized", "invalid_token",
                         "invalid_code",     "too_many_attempts",
                         "ip_banned",        "handshake_failed",
                         "hello_timeout"};
  for (const char* code : codes) {
    const auto route = classify(code, "");
    expect(route.where == surface::banner, "credential/transport -> banner");
    expect(route.level == tone::error, "banner failures are errors");
    expect(route.reconnect, "the banner offers a reconnect");
  }
  // The native shell's older name for a rejected code stays a banner error.
  expect(classify("invalid_code", "").where == surface::banner,
         "invalid_code is an alias for invalid_token");
  // A close reason with no code still counts as a handshake failure.
  expect(classify("", "hello timeout").where == surface::banner,
         "a handshake close reason reaches the banner");
  expect(classify("", "handshake failed").where == surface::banner,
         "a handshake failure reaches the banner");
  expect(classify("", "connection lost").where == surface::banner,
         "a lost transport reaches the banner");
}

void test_local_toast() {
  expect_toast("bad_input", "", tone::warn, "bad input is a warning toast");
  expect_toast("unknown_key", "", tone::warn,
               "an unknown key is a warning toast");
  expect_toast("bad_clipboard", "", tone::warn,
               "a clipboard failure is a warning toast");
  expect_toast("bad_file_action", "", tone::warn,
               "a file action failure is a warning toast");
  expect_toast("file_too_large", "", tone::warn,
               "file_* failures are warning toasts");
  expect_toast("clipboard_too_large", "", tone::warn,
               "a large clipboard is a warning toast");
  expect_toast("clipboard_files_staged", "", tone::info,
               "a staged clipboard file is an info toast");
  expect_toast("clipboard_files_uploaded", "", tone::info,
               "an uploaded clipboard file is an info toast");
  expect_toast("display_mode_failed", "", tone::error,
               "an unexpected host failure is an error toast");
  expect_toast("bad_logs_action", "", tone::error,
               "an unknown host error is an error toast");
  // `locked` and `access_denied` are not in the task's banner set.
  expect_toast("locked", "", tone::error,
               "a locked host stays a toast");
  expect_toast("access_denied", "", tone::error,
               "a denied request stays a toast");
}

}  // namespace

int main() {
  test_session_banner();
  test_local_toast();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
