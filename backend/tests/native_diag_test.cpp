// Native diagnostics: the same daily naming, 7-day retention, 32 MiB roll,
// tail filtering and redaction as the core's diag_log, plus the level gate.
// No Win32, no network.
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "native_diag.h"

namespace {

namespace fs = std::filesystem;
using molink::native_diag::component;
using molink::native_diag::field;
using molink::native_diag::level;
using molink::native_diag::logger;

int failures = 0;
int checks = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

fs::path make_temp_dir(const char* label) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = fs::temp_directory_path() /
                       (std::string("molink-native-diag-") + label + "-" +
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

void test_line_shape_and_components() {
  const std::string line = logger::format_line(
      0, level::info, component::session, "connect", "hello",
      {{"peer", "192.0.2.10"}});
  expect(contains(line, " INFO session.connect peer=192.0.2.10"),
         "line is timestamp LEVEL component.event key=value");
  expect(contains(line, ":: hello"), "the message follows ::");
  expect(contains(logger::format_line(0, level::debug, component::video,
                                      "mode", "", {}),
                  " DEBUG video.mode"),
         "debug lines keep their level and component");
  // An already namespaced event is not prefixed twice.
  expect(contains(logger::format_line(0, level::warn, component::session,
                                      "session.error", "", {}),
                  " WARN session.error"),
         "an event that already names its component stays unchanged");
  expect(logger::line_level(line) == level::info,
         "a formatted line reports its level");
}

void test_daily_file_name() {
  const int64_t noon = today_noon_ms();
  const std::string name = logger::file_name_for_day(noon);
  expect(name.size() == std::string("molink-YYYYMMDD.log").size(),
         "file name has the molink-YYYYMMDD.log shape");
  expect(name.rfind("molink-", 0) == 0, "file name starts with molink-");
  expect(name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0,
         "file name ends in .log");
}

void test_level_gate_and_filter() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("levels");
  logger log(dir.string(), true, 7, 1024 * 1024, [&now] { return now; }, 50);

  // Info and above are recorded by default; trace/debug are opt-in.
  log.log(level::debug, component::ui, "debug-default", "d");
  log.log(level::info, component::session, "info-default", "i");
  const std::string defaults = log.tail(10);
  expect(!contains(defaults, "ui.debug-default"),
         "debug is not recorded at the default info level");
  expect(contains(defaults, "session.info-default"),
         "info is recorded by default");

  log.set_level(level::trace);
  log.log(level::trace, component::ui, "trace", "t");
  log.log(level::debug, component::ui, "debug", "d");
  log.log(level::info, component::session, "info", "i");
  log.log(level::warn, component::files, "warn", "w");
  log.log(level::error, component::transport, "error", "e");
  const std::string all = log.tail(10);
  expect(contains(all, "ui.trace"), "trace is recorded after lowering the level");
  expect(contains(all, "transport.error"), "error is recorded");

  const std::string warn_up = log.tail(10, level::warn);
  expect(contains(warn_up, "files.warn"), "warn passes a warn filter");
  expect(contains(warn_up, "transport.error"),
         "error passes a warn filter");
  expect(!contains(warn_up, "session.info"), "info is filtered out");
  expect(!contains(warn_up, "ui.trace"), "trace is filtered out");

  // The recording gate drops lower levels from both the ring and the file.
  log.set_level(level::warn);
  log.log(level::info, component::session, "hidden", "not recorded");
  log.log(level::error, component::session, "kept", "recorded");
  const std::string after = log.tail(10);
  expect(!contains(after, "session.hidden"),
         "a line below the recording level is dropped");
  expect(contains(after, "session.kept"), "a line at the level is kept");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

void test_rotation_and_retention() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("retention");
  logger log(dir.string(), true, 7, 80, [&now] { return now; }, 100);
  log.log(level::info, component::ui, "first", "alpha");
  log.log(level::info, component::ui, "second", "beta");
  log.log(level::info, component::ui, "third", "gamma");
  expect(file_count(dir, "molink-") >= 2,
         "an oversized day rolls over into .1");

  for (int days = 8; days >= 1; --days) {
    now = noon - static_cast<int64_t>(days) * kDayMs;
    log.log(level::info, component::ui, "old",
            "day " + std::to_string(days));
  }
  now = noon;
  const std::size_t removed = log.cleanup_old_files();
  expect(removed >= 1, "files older than seven days are removed");
  const std::string day8 =
      logger::file_name_for_day(noon - 8 * kDayMs);
  expect(!fs::exists(dir / day8), "the eight-day-old file is gone");
  const std::string day6 =
      logger::file_name_for_day(noon - 6 * kDayMs);
  expect(fs::exists(dir / day6), "the six-day-old file is kept");
  expect(logger::keep_day(0, 7, 7), "the boundary day is kept");
  expect(!logger::keep_day(0, 8, 7), "one day past the window is removed");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

void test_default_32_mib_roll() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("32mib");
  logger log(dir.string(), true, 7, 32ull * 1024 * 1024,
             [&now] { return now; }, 50);
  std::vector<field> fields;
  for (int i = 0; i < 12; ++i) {
    fields.push_back({"f" + std::to_string(i), std::string(300, 'y')});
  }
  const std::string payload(900, 'z');
  for (int i = 0; i < 9000; ++i) {
    log.log(level::info, component::ui, "bulk", payload, fields);
  }
  const std::string name = logger::file_name_for_day(noon);
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
  logger log(dir.string(), true, 7, 8ull * 1024 * 1024,
             [&now] { return now; }, 50);
  std::string payload(1000, 'x');
  for (int i = 0; i < 300; ++i) {
    log.log(level::info, component::ui, "filler",
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
  logger log(dir.string(), true, 7, 1024 * 1024, [&now] { return now; }, 50);
  log.log(level::warn, component::auth, "test",
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
  expect(contains(text, "report.txt"), "a file name is kept");
  log.log(level::warn, component::input, "rejected",
          "unknown key code: MetaLeft");
  expect(contains(log.tail(20), "unknown key code: MetaLeft"),
         "a rejected key keeps the key name that makes it actionable");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

void test_disabled_never_writes() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("disabled");
  const fs::path target = dir / "logs";
  logger log(target.string(), false, 7, 1024, [&now] { return now; }, 50);
  log.log(level::info, component::ui, "quiet", "no file");
  expect(!fs::exists(target), "disabled logging does not create the directory");
  expect(contains(log.tail(10), "no file"),
         "the in-memory ring still answers a tail");

  log.set_enabled(true);
  log.log(level::info, component::ui, "loud", "file now");
  expect(fs::exists(target), "enabling creates the directory on demand");
  expect(contains(log.tail(10), "file now"), "the enabled line is readable");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

void test_info_and_clear() {
  const int64_t noon = today_noon_ms();
  int64_t now = noon;
  const fs::path dir = make_temp_dir("info");
  logger log(dir.string(), true, 7, 1024 * 1024, [&now] { return now; }, 50);
  log.log(level::info, component::ui, "hello", "world");
  const auto info = log.describe();
  expect(info.enabled, "info reports the recording switch");
  expect(info.retention_days == 7, "info reports the retention window");
  expect(info.directory == dir.string(), "info reports the directory");
  expect(info.current_bytes > 0, "info reports the current file size");
  expect(log.ensure_directory(), "the log directory exists");
  const std::size_t removed = log.clear();
  expect(removed >= 1, "clear removes the day file");
  expect(log.tail(10).empty(), "clear empties the ring");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace

int main() {
  test_line_shape_and_components();
  test_daily_file_name();
  test_level_gate_and_filter();
  test_rotation_and_retention();
  test_default_32_mib_roll();
  test_tail_limits_and_truncation();
  test_redaction();
  test_disabled_never_writes();
  test_info_and_clear();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
