// Clipboard-file rules driven without Win32 so they run on Linux next to the
// other protocol tests: limit clipping and omitted counts, 256 KiB range
// boundaries, the on-disk staging path, the retry budget, and echo keys.
#include <cstdio>
#include <string>
#include <vector>

#include "clipboard_files.h"

namespace {

using namespace molink::clipboard_files;

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

void expect_u64(std::uint64_t actual, std::uint64_t expected,
                const char* description) {
  ++checks;
  if (actual == expected) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n  expected: %llu\n  actual:   %llu\n",
               description, static_cast<unsigned long long>(expected),
               static_cast<unsigned long long>(actual));
}

entry file(std::uint32_t id, const std::string& name, std::uint64_t size) {
  entry item;
  item.id = id;
  item.name = name;
  item.size = size;
  return item;
}

void test_count_limit_and_omitted() {
  std::vector<entry> raw;
  for (std::uint32_t i = 1; i <= 10; ++i) {
    raw.push_back(file(i, "f" + std::to_string(i) + ".txt", 10));
  }
  const plan result = plan_pull_entries(raw);
  expect(result.files.size() == max_files, "only eight files are accepted");
  expect(result.omitted == 2, "the rest are reported as omitted");
  expect(result.bytes == 80, "the accepted byte count is summed");
  expect(result.files.front().name == "f1.txt", "the first prefix is kept");
  expect(result.files.back().name == "f8.txt", "the kept list is contiguous");
}

void test_per_file_limit_stops_the_prefix() {
  std::vector<entry> raw;
  raw.push_back(file(1, "small.txt", 10));
  raw.push_back(file(2, "huge.bin", max_file_bytes + 1));
  raw.push_back(file(3, "after.txt", 10));
  const plan result = plan_pull_entries(raw);
  expect(result.files.size() == 1, "the huge file ends the prefix");
  expect(result.omitted == 2, "the huge file and everything after are omitted");
  expect(result.files.front().name == "small.txt",
         "files before the huge one are still kept");
}

void test_total_limit_is_inclusive() {
  std::vector<entry> raw;
  raw.push_back(file(1, "a", 3));
  raw.push_back(file(2, "b", max_total_bytes - 3));
  raw.push_back(file(3, "c", 1));
  // Let the per-file budget be the total one, so this exercises the total
  // boundary rather than the 1 TiB per-file cap.
  const plan result =
      plan_entries(raw, max_files, max_total_bytes, max_total_bytes);
  expect(result.files.size() == 2, "a file that exactly reaches the total fits");
  expect(result.bytes == max_total_bytes, "the total boundary is inclusive");
  expect(result.omitted == 1, "one byte over the total is omitted");
}

void test_invalid_entries_are_omitted() {
  std::vector<entry> raw;
  raw.push_back(file(1, "ok.txt", 1));
  raw.push_back(file(0, "no-id.txt", 1));
  raw.push_back(file(2, "", 1));
  raw.push_back(file(3, "../escape.txt", 1));
  raw.push_back(file(4, "with:colon.txt", 1));
  const plan result = plan_pull_entries(raw);
  expect(result.files.size() == 1, "only the safe entry is planned");
  expect(result.files.front().name == "ok.txt", "the safe name survives");
  expect(result.omitted == 4, "invalid metadata is counted as omitted");
}

void test_duplicate_names_are_made_unique() {
  std::vector<entry> raw;
  raw.push_back(file(1, "a.txt", 1));
  raw.push_back(file(2, "A.TXT", 1));
  raw.push_back(file(3, "a.txt", 1));
  const plan result = plan_pull_entries(raw);
  expect(result.files.size() == 3, "all entries fit the budget");
  expect(result.files[0].name == "a.txt", "the first keeps its name");
  expect(result.files[1].name == "A (1).TXT",
         "a case-only clash gets a suffix before the extension");
  expect(result.files[2].name == "a (2).txt",
         "the next clash continues the suffix chain");
}

void test_sanitize_name_rejects_paths_and_devices() {
  expect(sanitize_name("a.txt") == "a.txt", "a plain name survives");
  expect(sanitize_name("../a.txt").empty(), "traversal is rejected");
  expect(sanitize_name("dir/a.txt").empty(), "a slash is rejected");
  expect(sanitize_name("C:\\a.txt").empty(), "a drive path is rejected");
  expect(sanitize_name("a?b*c.txt") == "abc.txt",
         "characters Windows cannot store are stripped");
  expect(sanitize_name("  name.txt  ") == "name.txt",
         "leading/trailing spaces are trimmed");
  expect(sanitize_name("..").empty(), "dot-only names are rejected");
  expect(sanitize_name("a\nb").size() == 2, "control characters are dropped");
}

void test_offer_plan_uses_lazy_caps() {
  std::vector<entry> raw;
  raw.push_back(file(1, "a", offer_max_total_bytes / 2));
  raw.push_back(file(2, "b", offer_max_total_bytes / 2 + 1));
  const plan result = plan_offer_entries(raw);
  expect(result.files.size() == 1, "the 2 GiB host cap trims the offer");
  expect(result.omitted == 1, "the over-cap offer entry is reported");
}

void test_chunk_boundaries() {
  expect_u64(chunk_length(0, 0), 0, "an empty file needs no request");
  expect_u64(chunk_length(1, 0), 1, "a one-byte file asks for one byte");
  expect_u64(chunk_length(chunk_bytes, 0), chunk_bytes,
             "a full chunk is 256 KiB");
  expect_u64(chunk_length(chunk_bytes + 1, 0), chunk_bytes,
             "the first range of a large file is a full chunk");
  expect_u64(chunk_length(chunk_bytes + 1, chunk_bytes), 1,
             "the last range is the remainder");
  expect_u64(chunk_length(3ull * 1024 * 1024, 0), chunk_bytes,
             "3 MiB starts with a full chunk");
  expect_u64(chunk_length(3ull * 1024 * 1024, 3ull * 1024 * 1024), 0,
             "offset == size means complete");
  expect_u64(chunk_length(10, 11), 0, "an offset past the end asks for nothing");
}

void test_chunk_validation() {
  expect(check_chunk(0, 0, 0, 4, 4) == chunk_state::ok,
         "a complete range is accepted");
  expect(check_chunk(2, 0, 0, 4, 0) == chunk_state::gone,
         "status 2 is gone even with no bytes");
  expect(check_chunk(1, 0, 0, 4, 0) == chunk_state::failed,
         "status 1 is retryable");
  expect(check_chunk(0, 4, 0, 4, 4) == chunk_state::incomplete,
         "a wrong offset is incomplete");
  expect(check_chunk(0, 0, 0, 4, 3) == chunk_state::incomplete,
         "a short read is never end-of-file");
  expect(check_chunk(0, 0, 0, 4, 5) == chunk_state::incomplete,
         "an over-long reply is incomplete");
}

void test_retry_budget() {
  range_retry retry;
  expect(retry.failed(), "the first failure is retried (attempt 2)");
  expect(retry.failed(), "the second failure is retried (attempt 3)");
  expect(!retry.failed(), "the third failure gives up");
  expect(retry.exhausted(), "the budget reports exhausted");
  expect(max_attempts == 3, "one request plus two retries");

  retry.succeeded();
  expect(retry.attempts == 0, "a good range resets the budget");
  expect(retry.failed(), "a later failure starts a fresh budget");
}

void test_staging_directory() {
  expect_eq(staging_relative_directory("abc-123", 1),
            "clipboard/abc-123/1",
            "the session and 1-based sequence name the folder");
  expect_eq(staging_relative_directory("../evil/../x", 7),
            "clipboard/_evil_.._x/7",
            "separators in the session label are neutralised");
  expect_eq(staging_relative_directory("..", 2), "clipboard/session/2",
            "a dots-only session label cannot name the parent directory");
  expect_eq(staging_relative_directory("", 2), "clipboard/session/2",
            "an empty session still has a safe folder");
  expect(staging_relative_directory("a", 0) == "clipboard/a/0",
         "sequence zero is rejected by the caller, not silently renamed");
}

void test_echo_paths_and_descriptors() {
  file_echo echo;
  echo.remember_path("C:\\Users\\Me\\Desktop\\A.TXT", 1000);
  expect(echo.suppress_path("c:/users/me/desktop/a.txt", 1500),
         "path matching ignores case and separators");
  expect(echo.suppress_path("C:\\\\Users\\\\Me\\\\Desktop\\\\A.TXT/", 1500),
         "repeated/trailing separators do not defeat the key");
  expect(!echo.suppress_path("C:/Users/Me/Desktop/b.txt", 1500),
         "a different path is not suppressed");

  echo.remember_descriptor("a.txt", 5, 2000);
  expect(echo.suppress_descriptor("A.TXT", 5, 2500),
         "descriptor matching is case-insensitive");
  expect(!echo.suppress_descriptor("a.txt", 6, 2500),
         "a size change is a different descriptor");
}

void test_echo_expiry_and_bound() {
  file_echo echo;
  echo.remember_path("/tmp/a", 1000);
  expect(echo.suppress_path("/tmp/a", 1000 + echo_ttl_ms - 1),
         "an entry is live just before two minutes");
  expect(!echo.suppress_path("/tmp/a", 1000 + echo_ttl_ms),
         "an entry expires at two minutes");
  expect(echo.size() == 0, "the expired entry is pruned");

  for (std::size_t i = 0; i < echo_max_entries + 10; ++i) {
    echo.remember_descriptor("f" + std::to_string(i), i, 1000 + i);
  }
  expect(echo.size() == echo_max_entries, "the echo table is bounded at 64");
  expect(echo.suppress_descriptor("f73", 73, 2000),
         "the newest entry is remembered");
  expect(!echo.suppress_descriptor("f0", 0, 2000),
         "the oldest entry was evicted");
}

}  // namespace

int main() {
  test_count_limit_and_omitted();
  test_per_file_limit_stops_the_prefix();
  test_total_limit_is_inclusive();
  test_invalid_entries_are_omitted();
  test_duplicate_names_are_made_unique();
  test_sanitize_name_rejects_paths_and_devices();
  test_offer_plan_uses_lazy_caps();
  test_chunk_boundaries();
  test_chunk_validation();
  test_retry_budget();
  test_staging_directory();
  test_echo_paths_and_descriptors();
  test_echo_expiry_and_bound();

  if (failures == 0) {
    std::printf("clipboard_files: %d checks passed\n", checks);
    return 0;
  }
  std::fprintf(stderr, "clipboard_files: %d of %d checks failed\n", failures,
               checks);
  return 1;
}
