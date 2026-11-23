// Clipboard text sync state machine, driven without Win32 so it runs on
// Linux next to the other protocol tests.
#include <cstdio>
#include <string>

#include "clipboard_sync.h"

namespace {

using namespace molink::clipboard;

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

std::u16string u16(const std::string& ascii) {
  return std::u16string(ascii.begin(), ascii.end());
}

void test_empty_text_is_not_sent() {
  text_sync sync;
  const decision empty = sync.on_local_text(u"", 1000);
  expect(!empty.send, "empty text is not sent");
  expect(empty.reason == skip_reason::empty, "empty text reports empty");
  expect(empty.bytes == 0, "empty text has zero bytes");
}

void test_duplicate_text_is_not_sent_twice() {
  text_sync sync;
  const decision first = sync.on_local_text(u"hello", 1000);
  expect(first.send && first.reason == skip_reason::none,
         "the first copy is sent");
  expect_eq(first.text, "hello", "the payload text is the clipboard text");
  expect(first.bytes == 5, "the log byte count is the UTF-8 length");

  const decision again = sync.on_local_text(u"hello", 2000);
  expect(!again.send, "the identical clipboard value is not sent again");
  expect(again.reason == skip_reason::duplicate,
         "the repeat is reported as duplicate");

  const decision changed = sync.on_local_text(u"world", 3000);
  expect(changed.send, "a different value is sent");

  // Web compares against the last value only, so going back to an earlier
  // value is a new selection, not a duplicate.
  const decision back = sync.on_local_text(u"hello", 4000);
  expect(back.send, "returning to an earlier value sends again");
}

void test_oversize_text_is_skipped() {
  text_sync sync;
  const std::u16string at_limit(max_text_bytes, u'a');
  const decision allowed = sync.on_local_text(at_limit, 1000);
  expect(allowed.send, "exactly 192 KiB of ASCII is allowed");
  expect(allowed.bytes == max_text_bytes, "the boundary is inclusive");

  const std::u16string over(max_text_bytes + 1, u'a');
  const decision skipped = sync.on_local_text(over, 2000);
  expect(!skipped.send, "text over 192 KiB is not sent");
  expect(skipped.reason == skip_reason::too_large,
         "the over-limit copy reports too_large");
  expect(skipped.bytes == max_text_bytes + 1, "the log keeps the real size");
}

void test_size_limit_counts_utf8_bytes() {
  text_sync sync;
  // Three UTF-8 bytes per CJK code point, so the cap is reached at 64 Ki
  // characters, exactly the unit the host's std::string check uses.
  const std::u16string cjk(65536, char16_t{0x4E16});
  const decision allowed = sync.on_local_text(cjk, 1000);
  expect(allowed.send, "192 KiB of 3-byte characters is allowed");
  expect(allowed.bytes == max_text_bytes, "the cap is measured in UTF-8 bytes");

  const std::u16string over(65537, char16_t{0x4E16});
  const decision skipped = sync.on_local_text(over, 2000);
  expect(!skipped.send && skipped.reason == skip_reason::too_large,
         "one more 3-byte character crosses the cap");
}

void test_host_echo_is_suppressed() {
  text_sync sync;
  sync.on_remote_text(u"from the host", 10'000);

  const decision echo = sync.on_local_text(u"from the host", 10'500);
  expect(!echo.send, "writing host text locally does not bounce back");
  expect(echo.reason == skip_reason::echo, "the bounce is reported as echo");
  expect(echo.bytes == 13, "the log still records the suppressed size");

  const decision other = sync.on_local_text(u"typed here", 11'000);
  expect(other.send, "a different local copy is still sent");
}

void test_echo_expires_after_two_minutes() {
  text_sync sync;
  const std::uint64_t started = 5'000;
  sync.on_remote_text(u"same text", started);

  expect(sync.echo_count() == 1, "the host write is remembered");

  const decision inside =
      sync.on_local_text(u"same text", started + echo_ttl_ms - 1);
  expect(!inside.send && inside.reason == skip_reason::echo,
         "the echo is still suppressed just before two minutes");

  const decision expired =
      sync.on_local_text(u"same text", started + echo_ttl_ms);
  expect(expired.send, "after two minutes the same text may be sent again");
  expect(expired.reason == skip_reason::none, "the expired echo is not a skip");
  expect(sync.echo_count() == 0, "the expired record is pruned");
}

void test_echo_records_are_per_text() {
  text_sync sync;
  sync.on_remote_text(u"alpha", 1000);
  const decision local_alpha = sync.on_local_text(u"alpha", 2000);
  expect(local_alpha.reason == skip_reason::echo, "alpha is an echo");

  const decision local_beta = sync.on_local_text(u"beta", 3000);
  expect(local_beta.send, "beta was never written by the host");
}

void test_echo_table_is_bounded() {
  text_sync sync;
  for (int i = 0; i < 80; ++i) {
    sync.on_remote_text(u16("remote-" + std::to_string(i)), 1000 + i);
  }
  expect(sync.echo_count() == echo_max_entries,
         "the echo table keeps at most 64 entries");
  const decision newest = sync.on_local_text(u"remote-79", 2000);
  expect(newest.reason == skip_reason::echo, "the newest entry is remembered");
  const decision oldest = sync.on_local_text(u"remote-0", 2000);
  expect(oldest.send, "the oldest entry was evicted");
}

void test_oversize_host_text_is_not_remembered() {
  text_sync sync;
  const std::u16string huge(max_text_bytes + 1, u'a');
  sync.on_remote_text(huge, 1000);
  expect(sync.echo_count() == 0,
         "an oversized host value never takes an echo slot");
}

void test_disabled_switch_stops_sends() {
  text_sync sync;
  sync.set_enabled(false);
  const decision off = sync.on_local_text(u"hello", 1000);
  expect(!off.send, "the switch stops automatic sends");
  expect(off.reason == skip_reason::disabled, "the skip reports disabled");

  sync.set_enabled(true);
  const decision on = sync.on_local_text(u"hello", 2000);
  expect(on.send, "turning the switch back on resumes sends");
}

void test_send_failure_can_retry() {
  text_sync sync;
  expect(sync.on_local_text(u"retry me", 1000).send, "first attempt is sent");
  expect(!sync.on_local_text(u"retry me", 2000).send,
         "an unchanged value is a duplicate");
  sync.reset_last_sent();
  expect(sync.on_local_text(u"retry me", 3000).send,
         "a failed send may retry the same text");
}

void test_payload_matches_web() {
  expect_eq(
      clipboard_sync_json(u"hi"),
      R"({"type":"clipboard-sync","text":"hi","hash":"683af69a","chain":""})",
      "the wire payload matches Client.sendClipboardSync");
  expect_eq(
      clipboard_sync_json(u"hello"),
      R"({"type":"clipboard-sync","text":"hello","hash":"4f9f2cab","chain":""})",
      "the hash is FNV-1a over UTF-16 code units");
  expect_eq(
      clipboard_sync_json(u"世界"),
      R"({"type":"clipboard-sync","text":"世界","hash":"e6b39eff","chain":""})",
      "non-ASCII text is sent as UTF-8 with the Web hash");
  expect(fnv1a_hex(u"hi") == "683af69a",
         "the hash is lower-case hex with no leading zeros");
}

void test_payload_escapes_json() {
  const std::u16string text = u"a\"b\\c\nd";
  expect_eq(
      clipboard_sync_json(text),
      R"({"type":"clipboard-sync","text":"a\"b\\c\nd","hash":"c3fcc83","chain":""})",
      "quotes, backslashes and newlines are escaped like JSON.stringify");
  expect_eq(clipboard_sync_json(u"\u0001"),
            R"({"type":"clipboard-sync","text":"\u0001","hash":"40c5b8c","chain":""})",
            "a control character becomes a \\u escape");
}

void test_utf16_to_utf8() {
  const std::u16string emoji = u"\U0001F600";
  expect(utf8_from_utf16(emoji) == "\xF0\x9F\x98\x80",
         "a surrogate pair becomes four UTF-8 bytes");
  // Written as code units: a \uD800 literal is a compile error in C++.
  expect(utf8_from_utf16(std::u16string{char16_t{0xD800}}) == "\xEF\xBF\xBD",
         "an unpaired high surrogate becomes U+FFFD");
  expect(utf8_from_utf16(std::u16string{char16_t{0xDC00}}) == "\xEF\xBF\xBD",
         "an unpaired low surrogate becomes U+FFFD");
}

void test_clear_echo() {
  text_sync sync;
  sync.on_remote_text(u"remembered", 1000);
  expect(sync.echo_count() == 1, "the record exists");
  sync.clear_echo();
  expect(sync.echo_count() == 0, "clear_echo empties the table");
  expect(sync.on_local_text(u"remembered", 2000).send,
         "a cleared echo no longer suppresses");
}

}  // namespace

int main() {
  test_empty_text_is_not_sent();
  test_duplicate_text_is_not_sent_twice();
  test_oversize_text_is_skipped();
  test_size_limit_counts_utf8_bytes();
  test_host_echo_is_suppressed();
  test_echo_expires_after_two_minutes();
  test_echo_records_are_per_text();
  test_echo_table_is_bounded();
  test_oversize_host_text_is_not_remembered();
  test_disabled_switch_stops_sends();
  test_send_failure_can_retry();
  test_payload_matches_web();
  test_payload_escapes_json();
  test_utf16_to_utf8();
  test_clear_echo();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
