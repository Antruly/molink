// Diagnostics: daily file naming, 7-day retention with a fake clock, size
// rotation, tail truncation and redaction. No libuvcpp, no network.
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "diag_log.h"

namespace {

namespace fs = std::filesystem;

int failures = 0;
int checks = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

/// A fresh empty directory per test.
fs::path make_temp_dir(const char* label) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = fs::temp_directory_path() /
                       (std::string("molink-diag-") + label + "-" +
                        std::to_string(stamp));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::size_t file_count(const fs::path& dir, const std::string& prefix) {
  std::size_t count = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (it->path().filename().string().rfind(prefix, 0) == 0) ++count;
  }
  return count;
}

/// Local noon today, in epoch ms. Keeps the day arithmetic away from a
/// midnight/DST boundary.
int64_t today_noon_ms() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  local.tm_hour = 12;
  local.tm_min = 0;
  local.tm_sec = 0;
  return static_cast<int64_t>(std::mktime(&local)) * 1000;
}

constexpr int64_t kDayMs = 24 * 60 * 60 * 1000ll;

void test_daily_file_name() {
  const int64_t noon = today_noon_ms();
  const std::string name = molink::diag_log::file_name_for_day(noon);
  expect(name.size() == std::string("molink-YYYYMMDD.log").size(),
         "file name has the molink-YYYYMMDD.log shape");
  expect(name.rfind("molink-", 0) == 0, "file name starts with molink-");
  expect(name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0,
         "file name ends in .log");
}

void test_rotation_and_retention() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("retention");
  // 80-byte cap so the second line rolls the file.
  molink::diag_log log(dir.string(), true, 7, 80,
                       [&now] { return now; }, 100);
  log.log(molink::diag_level::info, "first", "alpha");
  log.log(molink::diag_level::info, "second", "beta");
  log.log(molink::diag_level::info, "third", "gamma");
  expect(file_count(dir, "molink-") >= 2,
         "an oversized day rolls over into .1");

  // One file per day, eight days back, then clean up against "today".
  for (int days = 8; days >= 1; --days) {
    now = noon - static_cast<int64_t>(days) * kDayMs;
    log.log(molink::diag_level::info, "old", "day " + std::to_string(days));
  }
  now = noon;
  const std::size_t removed = log.cleanup_old_files();
  expect(removed >= 1, "files older than seven days are removed");
  const std::string day8 = molink::diag_log::file_name_for_day(noon - 8 * kDayMs);
  expect(!fs::exists(dir / day8), "the eight-day-old file is gone");
  const std::string day6 = molink::diag_log::file_name_for_day(noon - 6 * kDayMs);
  expect(fs::exists(dir / day6), "the six-day-old file is kept");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

void test_default_32_mib_roll() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("32mib");
  // The production default: a day file may not grow past 32 MiB.
  molink::diag_log log(dir.string(), true, 7, 32ull * 1024 * 1024,
                       [&now] { return now; }, 50);
  // A message is capped at 1000 characters, so fill the line with ordinary
  // fields as well: ~4.6 KiB per line, ~41 MiB total.
  std::vector<molink::diag_field> fields;
  for (int i = 0; i < 12; ++i) {
    fields.push_back({"f" + std::to_string(i), std::string(300, 'y')});
  }
  const std::string payload(900, 'z');
  for (int i = 0; i < 9000; ++i) {
    log.log(molink::diag_level::info, "bulk", payload, fields);
  }
  const std::string name = molink::diag_log::file_name_for_day(noon);
  std::error_code ec;
  const auto base_size = fs::file_size(dir / name, ec);
  expect(!ec && base_size <= 32ull * 1024 * 1024,
         "the active day file stays within 32 MiB");
  expect(fs::exists(dir / (name + ".1")),
         "the rolled-over 32 MiB file is kept as .1");
  fs::remove_all(dir, ec);
}

void test_tail_limits_and_truncation() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("tail");
  molink::diag_log log(dir.string(), true, 7, 8ull * 1024 * 1024,
                       [&now] { return now; }, 50);
  std::string payload(1000, 'x');
  for (int i = 0; i < 300; ++i) {
    log.log(molink::diag_level::info, "filler",
            "line " + std::to_string(i) + " " + payload);
  }
  const std::string tail = log.tail(500);
  expect(tail.size() <= 256ull * 1024 + 64,
         "tail stays within the 256 KiB budget");
  expect(tail.rfind("[truncated to 256 KiB]", 0) == 0,
         "a truncated tail says so");
  expect(contains(tail, "line 299"), "the newest line survives truncation");
  expect(!contains(tail, "line 0 "), "the oldest line is dropped first");

  const std::string two = log.tail(2);
  const std::size_t first = two.find('\n');
  expect(first != std::string::npos &&
             two.find('\n', first + 1) == std::string::npos,
         "tail(2) returns exactly two lines");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

void test_redaction() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("redact");
  molink::diag_log log(dir.string(), true, 7, 1024 * 1024,
                       [&now] { return now; }, 50);
  log.log(molink::diag_level::warn, "auth.test",
          "token=SECRET123456 hex=0123456789abcdef0123456789abcdef "
          "code=ABC-DEF-GHI",
          {{"token", "SECRETVALUE"},
           {"session", "RESUMETOKEN"},
           {"verification_code", "ZZZ-ZZZ-ZZZ"},
           {"peer", "192.0.2.10"},
           {"clip_text", "the clipboard body must never be written"},
           {"name", "report.txt"}});
  const std::string text = log.tail(20);
  expect(!contains(text, "SECRET123456"), "a labelled message token is masked");
  expect(!contains(text, "0123456789abcdef0123456789abcdef"),
         "a raw 32-hex session token is masked");
  expect(!contains(text, "ABC-DEF-GHI"), "a verification code shape is masked");
  expect(!contains(text, "SECRETVALUE"), "a token field is masked");
  expect(!contains(text, "RESUMETOKEN"), "a session field is masked");
  expect(!contains(text, "ZZZ-ZZZ-ZZZ"), "a code field is masked");
  expect(!contains(text, "clipboard body"),
         "clipboard text fields are masked");
  expect(contains(text, "192.0.2.10"), "the peer address is still useful");
  expect(contains(text, "report.txt"), "a file name is kept out of the test way");
  log.log(molink::diag_level::warn, "input.rejected",
          "unknown key code: MetaLeft");
  const std::string input = log.tail(20);
  expect(contains(input, "unknown key code: MetaLeft"),
         "a rejected key keeps the key name that makes it actionable");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

void test_disabled_never_writes() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("disabled");
  const fs::path target = dir / "logs";
  molink::diag_log log(target.string(), false, 7, 1024,
                       [&now] { return now; }, 50);
  log.log(molink::diag_level::info, "quiet", "no file");
  expect(!fs::exists(target), "disabled logging does not create the directory");
  expect(contains(log.tail(10), "no file"),
         "the in-memory ring still answers a tail");

  log.set_enabled(true);
  log.log(molink::diag_level::info, "loud", "file now");
  expect(fs::exists(target), "enabling creates the directory on demand");
  expect(contains(log.tail(10), "file now"), "the enabled line is readable");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

void test_info() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("info");
  molink::diag_log log(dir.string(), true, 7, 1024 * 1024,
                       [&now] { return now; }, 50);
  log.log(molink::diag_level::info, "hello", "world");
  const auto info = log.describe();
  expect(info.enabled, "info reports the recording switch");
  expect(info.retention_days == 7, "info reports the retention window");
  expect(info.directory == dir.string(), "info reports the directory");
  expect(info.current_bytes > 0, "info reports the current file size");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace

int main() {
  test_daily_file_name();
  test_rotation_and_retention();
  test_default_32_mib_roll();
  test_tail_limits_and_truncation();
  test_redaction();
  test_disabled_never_writes();
  test_info();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
