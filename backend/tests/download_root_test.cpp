// Storage-root rules for received files: the default root per platform, the
// device/date layout, unique file names, and the validation /api/settings
// applies before persisting a user-chosen root.
//
// The path builders are pure string code, so the layout and sanitisation cases
// run on every host. The filesystem cases use a private temp directory and
// never touch the real home directory.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include "download_root.h"
#include "util.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

int checks = 0;
int failures = 0;

void expect(bool value, const std::string& description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description.c_str());
}

void expect_eq(const std::string& actual, const std::string& expected,
               const std::string& description) {
  ++checks;
  if (actual == expected) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n  expected: [%s]\n  actual:   [%s]\n",
               description.c_str(), expected.c_str(), actual.c_str());
}

void set_env(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

fs::path make_temp_dir() {
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path dir = fs::temp_directory_path() /
                       ("molink-download-root-" + std::to_string(stamp));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

void touch(const fs::path& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << "x";
}

std::string joined(const fs::path& path) { return molink::path_to_utf8(path); }

void test_sanitize_device_component() {
  expect_eq(molink::sanitize_device_component("CBM-RPH-3NZ"), "CBM-RPH-3NZ",
            "a normal device id is untouched");
  expect_eq(molink::sanitize_device_component("192.168.2.20"),
            "192.168.2.20", "an address used as a fallback keeps its dots");
  expect_eq(molink::sanitize_device_component("a\\b/c:d*e?f\"g<h>i|j"),
            "a_b_c_d_e_f_g_h_i_j",
            "every Windows-illegal character becomes an underscore");
  expect_eq(molink::sanitize_device_component(std::string("bad\x01name\x7f")),
            "bad_name_", "control characters become underscores");
  expect_eq(molink::sanitize_device_component("设备 一"), "设备 一",
            "Chinese text and spaces survive");
  expect_eq(molink::sanitize_device_component(".."), "unknown",
            "a parent reference cannot become a device directory");
  expect_eq(molink::sanitize_device_component(""), "unknown",
            "an empty id gets a stable placeholder");

  const std::string long_ascii(200, 'x');
  const std::string cut = molink::sanitize_device_component(long_ascii);
  expect(cut.size() == 64, "a long ASCII id is truncated to 64 bytes");

  std::string long_utf8;
  for (int i = 0; i < 40; ++i) long_utf8 += "设";
  const std::string utf8_cut = molink::sanitize_device_component(long_utf8);
  expect(utf8_cut.size() <= 64, "a long UTF-8 id fits the byte limit");
  // 21 three-byte characters = 63 bytes; a 22nd would overflow 64 and must
  // not be split into an invalid sequence.
  expect(utf8_cut.size() == 63, "truncation stops on a UTF-8 boundary");
}

void test_unique_path(const fs::path& dir) {
  touch(dir / "a.txt");
  expect_eq(molink::unique_path(joined(dir), "a.txt"),
            joined(dir / "a (1).txt"), "the first collision becomes (1)");
  touch(dir / "a (1).txt");
  expect_eq(molink::unique_path(joined(dir), "a.txt"),
            joined(dir / "a (2).txt"), "the second collision becomes (2)");

  touch(dir / "a.tar.gz");
  expect_eq(molink::unique_path(joined(dir), "a.tar.gz"),
            joined(dir / "a.tar (1).gz"),
            "only the last extension is kept after the counter");
}

void test_default_download_root() {
  const fs::path home = fs::temp_directory_path() /
                        ("molink-home-" +
                         std::to_string(
                             std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count()));
#if defined(_WIN32)
  set_env("USERPROFILE", joined(home));
#else
  set_env("HOME", joined(home));
#endif
  expect_eq(molink::default_download_root(), joined(home / "moLink"),
            "the default root is the platform home plus moLink");
}

void test_validation(const fs::path& dir) {
  using molink::download_root_error;

  expect(molink::validate_download_root("relative/moLink").error ==
             download_root_error::relative,
         "a relative root is refused");
  expect(molink::validate_download_root("").error ==
             download_root_error::empty,
         "an empty root is refused");
#if defined(_WIN32)
  expect(molink::validate_download_root("C:\\Windows\\Temp").error ==
             download_root_error::system_directory,
         "the Windows directory is refused");
#else
  expect(molink::validate_download_root("/usr/local/molink").error ==
             download_root_error::system_directory,
         "a path under /usr is refused");
  expect(molink::validate_download_root("/etc").error ==
             download_root_error::system_directory,
         "/etc is refused");
  expect(molink::validate_download_root("/").error ==
             download_root_error::system_directory,
         "the filesystem root is refused");
#endif
  const std::string program = molink::program_directory();
  expect(!program.empty(), "the program directory is resolvable");
  if (!program.empty()) {
    expect(molink::validate_download_root(program).error ==
               download_root_error::install_directory,
           "the program install directory is refused");
  }

  const fs::path store = dir / "moLink 中文";
  std::error_code ec;
  fs::create_directories(store, ec);
  expect(molink::validate_download_root(joined(store)).ok,
         "an existing writable directory with spaces and Chinese is accepted");

  const fs::path pending = dir / "not-created" / "deep";
  expect(molink::validate_download_root(joined(pending)).ok,
         "a missing leaf under a writable parent is accepted");
  expect(!fs::exists(dir / "not-created"),
         "validation does not pre-create the directory");

  const fs::path file = dir / "plain-file";
  touch(file);
  expect(molink::validate_download_root(joined(file)).error ==
             download_root_error::not_directory,
         "an existing file is not a storage root");

#if !defined(_WIN32)
  const fs::path locked = dir / "locked";
  fs::create_directories(locked, ec);
  ::chmod(locked.c_str(), 0555);
  const bool root_user = ::geteuid() == 0;
  const auto locked_check = molink::validate_download_root(joined(locked));
  ::chmod(locked.c_str(), 0755);
  if (!root_user) {
    expect(locked_check.error == download_root_error::not_writable,
           "an unwritable directory is refused");
  }
#endif
}

void test_layout() {
  const fs::path root = fs::path("/home/user") / "moLink";
  expect_eq(molink::download_directory(joined(root), "CBM-RPH-3NZ",
                                       "2026-11-26"),
            joined(root / "CBM-RPH-3NZ" / "2026-11-26"),
            "received files land under <root>/<id>/<date>");
  expect_eq(molink::chat_attachment_dir(joined(root), "CBM-RPH-3NZ",
                                       "2026-11-26"),
            joined(root / "CBM-RPH-3NZ" / "2026-11-26" / "chat"),
            "chat attachments get the chat subdirectory");
  expect_eq(molink::download_directory(joined(root), "a:b/c", "2026-11-26"),
            joined(root / "a_b_c" / "2026-11-26"),
            "an illegal device id is sanitized inside the layout");
  expect_eq(molink::download_directory(joined(root), "192.168.2.20",
                                       "2026-11-26"),
            joined(root / "192.168.2.20" / "2026-11-26"),
            "an address stands in for a missing device id");
  expect_eq(molink::download_directory(joined(root), "id", ".."),
            joined(root / "id" / "unknown"),
            "a malformed date cannot escape the root");
}

void test_local_date() {
  std::tm local{};
  local.tm_year = 2026 - 1900;
  local.tm_mon = 11 - 1;
  local.tm_mday = 26;
  local.tm_hour = 23;
  local.tm_min = 30;
  local.tm_isdst = -1;
  const std::time_t when = std::mktime(&local);
  expect_eq(molink::local_date_string(when), "2026-11-26",
            "the date uses the local timezone and YYYY-MM-DD");

  const std::string today = molink::local_date_string();
  expect(today.size() == 10 && today[4] == '-' && today[7] == '-',
         "the default date has the YYYY-MM-DD shape");
}

}  // namespace

int main() {
  const fs::path dir = make_temp_dir();
  test_sanitize_device_component();
  fs::create_directories(dir / "unique");
  test_unique_path(dir / "unique");
  test_default_download_root();
  test_validation(dir);
  test_layout();
  test_local_date();

  std::error_code ec;
  fs::remove_all(dir, ec);
  std::printf("download_root: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
