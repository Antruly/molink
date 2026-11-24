// Clipboard owner classification, active-session routing and the per-session
// echo table. All three are pure data so they run on Linux next to the other
// protocol tests; the platform calls that produce the owner only live in the
// X11/Windows/macOS backends.
#include <cstdio>
#include <string>
#include <vector>

#include "clipboard/clipboard_route.h"

namespace {

using namespace molink;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_eq(const std::string& actual, const std::string& expected,
               const char* description) {
  ++checks;
  if (actual == expected) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n",
               description, expected.c_str(), actual.c_str());
}

/// The tests use "now = 10'000" almost everywhere. A foreground session gets
/// a lease from five seconds before that, so it is fresh under the 15 s TTL.
constexpr uint64_t kTestNow = 10'000;

clipboard_route_session session(uint32_t id, bool authenticated = true,
                                bool foreground = false,
                                uint64_t input_ms = 0,
                                uint64_t connected_ms = 1000) {
  clipboard_route_session value;
  value.id = id;
  value.authenticated = authenticated;
  value.connected = true;
  value.foreground_ms = foreground ? kTestNow - 5'000 : 0;
  value.last_input_ms = input_ms;
  value.connected_ms = connected_ms;
  return value;
}

void test_owner_names() {
  expect_eq(clipboard_owner_name(clipboard_owner::self), "self",
            "self owner is logged as self");
  expect_eq(clipboard_owner_name(clipboard_owner::external), "external",
            "another application is external");
  expect_eq(clipboard_owner_name(clipboard_owner::unknown), "unknown",
            "an unnamed owner is unknown");
}

void test_no_sessions_has_no_targets() {
  const clipboard_route route = select_clipboard_targets({}, 10'000);
  expect(route.targets.empty(), "no session means no delivery");
  expect(route.active == 0, "no session means no active id");
  expect(route.reason == clipboard_route_reason::none,
         "the route reports none");
}

void test_unauthenticated_and_disconnected_are_skipped() {
  std::vector<clipboard_route_session> sessions;
  clipboard_route_session pending = session(1);
  pending.authenticated = false;
  sessions.push_back(pending);
  clipboard_route_session gone = session(2);
  gone.connected = false;
  sessions.push_back(gone);
  const clipboard_route route = select_clipboard_targets(sessions, 10'000);
  expect(route.targets.empty(),
         "only authenticated, connected sessions are targets");
  expect(route.reason == clipboard_route_reason::none,
         "no candidate reports none");
}

void test_one_viewer_is_sole() {
  const clipboard_route route =
      select_clipboard_targets({session(7)}, 10'000);
  expect(route.targets.size() == 1 && route.targets[0] == 7,
         "a lone viewer is the target");
  expect(route.active == 7, "the lone viewer is active");
  expect(route.reason == clipboard_route_reason::sole,
         "the reason is sole");
}

void test_foreground_wins_over_input() {
  std::vector<clipboard_route_session> sessions;
  sessions.push_back(session(1, true, false, 9'000));
  sessions.push_back(session(2, true, true, 0));
  const clipboard_route route =
      select_clipboard_targets(sessions, 10'000);
  expect(route.targets.size() == 1 && route.targets[0] == 2,
         "the viewer that marked itself foreground wins");
  expect(route.active == 2, "foreground session is active");
  expect(route.reason == clipboard_route_reason::foreground,
         "the reason records the foreground signal");
}

void test_two_foregrounds_pick_the_most_recent_input() {
  std::vector<clipboard_route_session> sessions;
  sessions.push_back(session(1, true, true, 5'000, 100));
  sessions.push_back(session(2, true, true, 8'000, 100));
  const clipboard_route route =
      select_clipboard_targets(sessions, 10'000);
  expect(route.targets.size() == 1 && route.targets[0] == 2,
         "two visible windows route to the one that was used last");
  expect(route.reason == clipboard_route_reason::foreground,
         "still a foreground route");
}

void test_input_fallback_uses_recent_input() {
  std::vector<clipboard_route_session> sessions;
  sessions.push_back(session(1, true, false, 4'000));
  sessions.push_back(session(2, true, false, 9'000));
  const clipboard_route route =
      select_clipboard_targets(sessions, 10'000);
  expect(route.targets.size() == 1 && route.targets[0] == 2,
         "without foreground the latest input wins");
  expect(route.reason == clipboard_route_reason::input,
         "the reason records input");
}

void test_stale_input_has_no_target_by_default() {
  std::vector<clipboard_route_session> sessions;
  sessions.push_back(session(1, true, false, 1'000));
  sessions.push_back(session(2, true, false, 2'000));
  const clipboard_route route = select_clipboard_targets(
      sessions, 2'000 + kClipboardInputActiveMs + 1);
  expect(route.targets.empty(),
         "two viewers with no current activity must not be broadcast to");
  expect(route.reason == clipboard_route_reason::no_target,
         "the diagnostic records that nothing was a target");
}

void test_broadcast_requires_the_explicit_opt_in() {
  std::vector<clipboard_route_session> sessions;
  sessions.push_back(session(1, true, false, 1'000));
  sessions.push_back(session(2, true, false, 2'000));
  const uint64_t now = 2'000 + kClipboardInputActiveMs + 1;
  const clipboard_route route = select_clipboard_targets(
      sessions, now, kClipboardInputActiveMs, true);
  expect(route.targets.size() == 2,
         "the opt-in fans out to every authenticated viewer");
  expect(route.reason == clipboard_route_reason::broadcast,
         "the opt-in fallback is explicit in the diagnostic");
}

void test_input_window_boundary() {
  std::vector<clipboard_route_session> sessions;
  sessions.push_back(session(1, true, false, 1'000));
  sessions.push_back(session(2, true, false, 2'000));
  const clipboard_route at_edge = select_clipboard_targets(
      sessions, 2'000 + kClipboardInputActiveMs);
  expect(at_edge.targets.size() == 1 && at_edge.targets[0] == 2,
         "input exactly at the window edge still counts");

  const clipboard_route past_edge = select_clipboard_targets(
      sessions, 2'000 + kClipboardInputActiveMs + 1);
  expect(past_edge.reason == clipboard_route_reason::no_target,
         "one millisecond past the edge is stale, so nothing is targeted");
}

void test_foreground_lease_expires() {
  std::vector<clipboard_route_session> sessions;
  clipboard_route_session fresh = session(1, true, true);
  // The helper backdates the lease by 5 s; this test needs it to start now so
  // the TTL edge is exactly at kTestNow + ttl.
  fresh.foreground_ms = kTestNow;
  fresh.last_input_ms = kTestNow;
  sessions.push_back(fresh);
  // A second viewer is connected so an expired lease cannot hide behind the
  // lone-viewer rule.
  sessions.push_back(session(2, true, false, 1'000));

  const clipboard_route live =
      select_clipboard_targets(sessions, kTestNow + kClipboardForegroundTtlMs);
  expect(live.reason != clipboard_route_reason::foreground,
         "a lease exactly at the TTL is no longer foreground");

  const clipboard_route just_before = select_clipboard_targets(
      sessions, kTestNow + kClipboardForegroundTtlMs - 1);
  expect(just_before.reason == clipboard_route_reason::foreground,
         "one millisecond before the TTL the lease is still valid");
}

void test_expired_foreground_does_not_win_over_sole_viewer() {
  // Lease starts at kTestNow - 5 s; reading the route a full TTL later makes
  // it stale.
  const clipboard_route route = select_clipboard_targets(
      {session(1, true, true)}, kTestNow + kClipboardForegroundTtlMs);
  expect(route.reason == clipboard_route_reason::sole,
         "an expired lease falls through to the sole-viewer rule");
  expect(route.targets.size() == 1 && route.targets[0] == 1,
         "the only viewer is still the target by the sole rule");
}

void test_new_change_reports_identical_content() {
  // A platform counter advanced: this is a new SetClipboardData, so even the
  // exact same bytes must be reported (the second copy of a password).
  expect(clipboard_watcher_should_report(true, "same", "same"),
         "a new change with identical content is still reported");
  expect(clipboard_watcher_should_report(false, "same", "same") == false,
         "without a new change, identical content is the fallback no-op");
  expect(clipboard_watcher_should_report(false, "other", "same"),
         "without a counter, changed content is reported");
  expect(clipboard_watcher_should_report(true, "", "") == false,
         "a new change with no text is not reported as text");
}

void test_paths_only_reach_loopback_peers() {
  expect(clipboard_peer_may_see_paths("127.0.0.1"),
         "the local page may see a host path");
  expect(clipboard_peer_may_see_paths("::1"),
         "IPv6 loopback may see a host path");
  expect(clipboard_peer_may_see_paths("::ffff:127.0.0.1"),
         "the IPv4-mapped loopback form may see a host path");
  expect(!clipboard_peer_may_see_paths("192.168.2.10"),
         "a LAN viewer never receives a host absolute path");
  expect(!clipboard_peer_may_see_paths("203.0.113.7"),
         "a remote viewer never receives a host absolute path");
}

void test_echo_is_per_session() {
  clipboard_echo_table echoes(60'000, 64);
  const std::string key = clipboard_text_key("deadbeef", 5);
  echoes.remember(1, key, clipboard_echo_direction::delivered, 1'000);

  expect(echoes.suppress(1, key, 1'000),
         "the session that received the value sees an echo");
  expect(!echoes.suppress(2, key, 1'000),
         "another session's genuine copy of the same value is not suppressed");
  expect(echoes.size() == 1, "one entry exists");
}

void test_echo_expires() {
  clipboard_echo_table echoes(60'000, 64);
  const std::string key = clipboard_text_key("cafe", 4);
  echoes.remember(3, key, clipboard_echo_direction::received, 5'000);
  expect(echoes.suppress(3, key, 64'000),
         "the echo is suppressed just before the TTL");
  expect(!echoes.suppress(3, key, 65'000),
         "the echo expires at the TTL boundary");
  expect(echoes.size() == 0, "the expired entry is pruned");
}

void test_echo_table_is_bounded_and_newest_last() {
  clipboard_echo_table echoes(60'000, 3);
  for (uint32_t i = 0; i < 5; ++i) {
    echoes.remember(1, "key-" + std::to_string(i),
                    clipboard_echo_direction::delivered, 1'000 + i);
  }
  expect(echoes.size() == 3, "the table keeps at most max_entries");
  expect(!echoes.suppress(1, "key-0", 2'000), "the oldest entries were evicted");
  expect(echoes.suppress(1, "key-4", 2'000), "the newest entry is remembered");
}

void test_forget_session_drops_only_that_sessions_entries() {
  clipboard_echo_table echoes;
  echoes.remember(1, "a", clipboard_echo_direction::delivered, 1'000);
  echoes.remember(2, "a", clipboard_echo_direction::delivered, 1'000);
  echoes.forget_session(1);
  expect(!echoes.suppress(1, "a", 2'000), "the closed session is forgotten");
  expect(echoes.suppress(2, "a", 2'000),
         "the other session's entry survives a close");
  echoes.clear();
  expect(!echoes.suppress(2, "a", 2'000), "clear empties the table");
}

void test_keys_distinguish_kinds_and_lengths() {
  expect_eq(clipboard_text_key("abc", 5), "text:abc:5",
            "the text key includes the hash and byte length");
  expect(clipboard_text_key("abc", 6) != clipboard_text_key("abc", 5),
         "the length separates hash collisions");
  expect_eq(clipboard_files_key("a.txt\n"), "files:a.txt\n",
            "the file key is namespaced separately");
  expect(clipboard_files_key("x") != clipboard_text_key("x", 1),
         "a file signature cannot collide with text");
}

void test_route_reason_names() {
  expect_eq(clipboard_route_reason_name(clipboard_route_reason::foreground),
            "foreground", "foreground spelling");
  expect_eq(clipboard_route_reason_name(clipboard_route_reason::input),
            "input", "input spelling");
  expect_eq(clipboard_route_reason_name(clipboard_route_reason::sole),
            "sole", "sole spelling");
  expect_eq(clipboard_route_reason_name(clipboard_route_reason::broadcast),
            "broadcast", "broadcast spelling");
  expect_eq(clipboard_route_reason_name(clipboard_route_reason::no_target),
            "no_target", "no_target spelling");
}

}  // namespace

int main() {
  test_owner_names();
  test_no_sessions_has_no_targets();
  test_unauthenticated_and_disconnected_are_skipped();
  test_one_viewer_is_sole();
  test_foreground_wins_over_input();
  test_two_foregrounds_pick_the_most_recent_input();
  test_input_fallback_uses_recent_input();
  test_stale_input_has_no_target_by_default();
  test_broadcast_requires_the_explicit_opt_in();
  test_input_window_boundary();
  test_foreground_lease_expires();
  test_expired_foreground_does_not_win_over_sole_viewer();
  test_new_change_reports_identical_content();
  test_paths_only_reach_loopback_peers();
  test_echo_is_per_session();
  test_echo_expires();
  test_echo_table_is_bounded_and_newest_last();
  test_forget_session_drops_only_that_sessions_entries();
  test_keys_distinguish_kinds_and_lengths();
  test_route_reason_names();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
