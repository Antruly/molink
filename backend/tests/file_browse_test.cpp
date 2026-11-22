// Path rules and volume/root enumeration for the file panel's "browse any
// directory" mode. The production path code is pure string handling, so the
// Windows cases run on every platform.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <webapp/uvcpp_web_json.h>

#include "directory_listing.h"
#include "file_transfer.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using uvcpp::uvcpp_json;

int checks = 0;
int failures = 0;

void expect(bool value, const char* message) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

void expect_eq(const std::optional<std::string>& actual,
               const std::string& expected, const char* message) {
  expect(actual.has_value() && *actual == expected, message);
  if (actual && *actual != expected) {
    std::printf("  expected <%s>, got <%s>\n", expected.c_str(),
                actual->c_str());
  }
}

void expect_null(const std::optional<std::string>& actual,
                 const char* message) {
  expect(!actual.has_value(), message);
}

void set_home(const std::string& home) {
#if defined(_WIN32)
  _putenv_s("USERPROFILE", home.c_str());
#else
  setenv("HOME", home.c_str(), 1);
#endif
}

std::string native(const fs::path& path) { return path.string(); }

void test_posix_normalization() {
  const std::string home = "/home/me";
  expect_null(molink::fslist::normalize_posix_path("", home),
              "empty input is not a path");
  expect_eq(molink::fslist::normalize_posix_path("/", home), "/",
            "POSIX root stays root");
  expect_eq(molink::fslist::normalize_posix_path("/var/log", home),
            "/var/log", "absolute POSIX path is kept");
  expect_eq(molink::fslist::normalize_posix_path("/var/../etc/./x", home),
            "/etc/x", "POSIX . and .. collapse");
  expect_eq(molink::fslist::normalize_posix_path("/../../etc", home), "/etc",
            ".. cannot climb above /");
  expect_eq(molink::fslist::normalize_posix_path("docs/../pics", home),
            "/home/me/pics", "relative path resolves against home");
  expect_eq(molink::fslist::normalize_posix_path("~", home), home,
            "~ is home");
  expect_eq(molink::fslist::normalize_posix_path("~/Documents", home),
            "/home/me/Documents", "~/x is home-relative");
  expect_null(molink::fslist::normalize_posix_path("/bad\nname", home),
              "control characters are rejected");
  expect_null(molink::fslist::normalize_posix_path(std::string("/a") + '\0' +
                                                       "b",
                                                   home),
              "NUL is rejected");
  // A backslash is an ordinary POSIX file-name character.
  expect_eq(molink::fslist::normalize_posix_path("/tmp/a\\b", home),
            "/tmp/a\\b", "POSIX keeps backslashes in a name");
}

void test_windows_normalization() {
  const std::string home = "C:\\Users\\me";
  expect_null(molink::fslist::normalize_windows_path("", home),
              "empty Windows input is not a path");
  expect_eq(molink::fslist::normalize_windows_path("C:\\", home), "C:\\",
            "drive root keeps its separator");
  expect_eq(molink::fslist::normalize_windows_path("C:/", home), "C:\\",
            "forward-slash drive root normalises");
  expect_eq(molink::fslist::normalize_windows_path("C:/Windows/System32", home),
            "C:\\Windows\\System32", "mixed separators normalise");
  expect_eq(molink::fslist::normalize_windows_path(
                "c:\\windows\\..\\Windows", home),
            "C:\\Windows", "drive letter is upper-cased and .. collapses");
  expect_eq(molink::fslist::normalize_windows_path("\\\\server\\share", home),
            "\\\\server\\share", "UNC root is kept");
  expect_eq(molink::fslist::normalize_windows_path(
                "//server/share/x", home),
            "\\\\server\\share\\x", "forward-slash UNC normalises");
  expect_eq(molink::fslist::normalize_windows_path("\\Windows", home),
            "C:\\Windows", "a single leading separator uses the home drive");
  expect_eq(molink::fslist::normalize_windows_path("..\\..\\Windows", home),
            "C:\\Windows", "relative .. resolves against home");
  expect_eq(molink::fslist::normalize_windows_path("Documents", home),
            "C:\\Users\\me\\Documents", "relative path resolves against home");
  expect_eq(molink::fslist::normalize_windows_path("~\\Desktop", home),
            "C:\\Users\\me\\Desktop", "~ is home");
  expect_null(molink::fslist::normalize_windows_path("C:", home),
              "drive-relative C: is ambiguous");
  expect_null(molink::fslist::normalize_windows_path("\\\\server", home),
              "UNC without a share is rejected");
  expect_null(molink::fslist::normalize_windows_path("C:\\bad<name", home),
              "illegal Windows name characters are rejected");
  expect_null(molink::fslist::normalize_windows_path("\\\\?\\C:\\x", home),
              "device paths are not browsable folders");
}

void test_roots_from_drives() {
  uint32_t mask = 0;
  mask |= 1u << 0;  // A removable
  mask |= 1u << 2;  // C fixed
  mask |= 1u << 3;  // D network
  mask |= 1u << 4;  // E cdrom
  mask |= 1u << 5;  // F ramdisk
  std::vector<int> types(26, 0);
  types[0] = 2;
  types[2] = 3;
  types[3] = 4;
  types[4] = 5;
  types[5] = 6;
  const auto roots = molink::fslist::roots_from_drives(
      mask, [&types](int index) { return types[index]; },
      "C:\\Users\\me");
  expect(roots.size() == 6, "five drives plus the home shortcut");
  if (roots.size() == 6) {
    expect(roots[0].name == "A:" && roots[0].path == "A:\\",
           "removable drive name/path");
    expect(std::string(molink::fslist::root_kind_name(roots[0].kind)) ==
               "removable",
           "removable drive kind");
    expect(std::string(molink::fslist::root_kind_name(roots[1].kind)) ==
               "fixed",
           "fixed drive kind");
    expect(std::string(molink::fslist::root_kind_name(roots[2].kind)) ==
               "network",
           "mapped network drive is marked network");
    expect(std::string(molink::fslist::root_kind_name(roots[3].kind)) ==
               "cdrom",
           "cdrom drive kind");
    expect(std::string(molink::fslist::root_kind_name(roots[4].kind)) == "ram",
           "ramdisk kind");
    expect(roots[5].kind == molink::fslist::root_kind::home &&
               roots[5].path == "C:\\Users\\me",
           "home shortcut is last");
  }
  const auto none =
      molink::fslist::roots_from_drives(0, [](int) { return 0; }, "");
  expect(none.empty(), "no drive bits and no home means no roots");
  const auto posix = molink::fslist::system_roots("/home/me");
#if defined(_WIN32)
  expect(!posix.empty(), "Windows enumerates at least one drive");
#else
  expect(posix.size() == 2, "POSIX roots are / and home");
  expect(posix[0].path == "/", "POSIX root is /");
  expect(posix[1].path == "/home/me", "POSIX home shortcut");
#endif
}

void test_list_and_walk() {
  const fs::path base =
      fs::temp_directory_path() / "molink-directory-listing-test";
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base / "alpha", ec);
  fs::create_directories(base / "beta", ec);
  {
    std::ofstream file(base / "note.txt", std::ios::binary);
    file << "hello";
  }
  {
    std::ofstream file(base / ".hidden", std::ios::binary);
    file << "x";
  }
#if !defined(_WIN32)
  fs::create_directory_symlink(base / "alpha", base / "link-alpha", ec);
#endif
  std::string why;
  const auto listed =
      molink::fslist::list_directory(native(base), why);
  expect(listed.has_value(), "list_directory succeeds on a temp folder");
  if (listed) {
    expect(listed->path == native(base), "listing reports the native path");
    expect(!listed->parent.empty(), "listing reports a parent");
    bool saw_dir = false;
    bool saw_file = false;
    bool saw_hidden = false;
    bool saw_link = false;
    for (const auto& entry : listed->entries) {
      if (entry.name == "alpha") saw_dir = entry.dir;
      if (entry.name == "note.txt") {
        saw_file = !entry.dir && entry.size == 5 && entry.mtime > 0;
      }
      if (entry.name == ".hidden") saw_hidden = entry.hidden;
      if (entry.name == "link-alpha") saw_link = entry.link && entry.dir;
    }
    expect(saw_dir && saw_file, "folders and files are reported");
    expect(saw_hidden, "dot files are marked hidden");
#if !defined(_WIN32)
    expect(saw_link, "symlinked folders are marked as links but enterable");
#endif
    expect(listed->entries.front().dir, "folders sort before files");
  }

  why.clear();
  const auto missing =
      molink::fslist::list_directory(native(base / "missing"), why);
  expect(!missing.has_value() && why == "no such folder",
         "a missing folder reports a readable error");

  why.clear();
  std::ofstream plain(base / "plain.bin", std::ios::binary);
  plain << "x";
  plain.close();
  const auto not_dir =
      molink::fslist::list_directory(native(base / "plain.bin"), why);
  expect(!not_dir.has_value() && why == "not a folder",
         "a file is not listed as a folder");

#if !defined(_WIN32)
  if (geteuid() != 0) {
    fs::create_directories(base / "denied", ec);
    ::chmod((base / "denied").c_str(), 0);
    why.clear();
    const auto denied =
        molink::fslist::list_directory(native(base / "denied"), why);
    ::chmod((base / "denied").c_str(), 0700);
    expect(!denied.has_value() && why == "permission denied",
           "an unreadable folder reports permission denied");
  }
#endif

  // Walk: nested files keep '/'-separated relative paths, and a symlinked
  // folder is reported but not descended into (so a loop terminates).
  fs::create_directories(base / "alpha" / "nested", ec);
  {
    std::ofstream file(base / "alpha" / "nested" / "deep.txt",
                       std::ios::binary);
    file << "deep";
  }
#if !defined(_WIN32)
  fs::create_directory_symlink(base, base / "alpha" / "loop", ec);
#endif
  const auto walked = molink::fslist::walk_directory(native(base), 64, 8);
  bool saw_deep = false;
  bool loop_link = false;
  for (const auto& entry : walked.entries) {
    if (entry.path == "alpha/nested/deep.txt" && !entry.dir &&
        entry.size == 4) {
      saw_deep = true;
    }
    if (entry.path == "alpha/loop") loop_link = entry.link && entry.dir;
  }
  expect(saw_deep, "walk keeps relative sub-paths");
#if !defined(_WIN32)
  expect(loop_link, "walk marks a symlink loop without following it");
#endif
  const auto capped = molink::fslist::walk_directory(native(base), 2, 8);
  expect(capped.entries.size() == 2 && capped.truncated,
         "walk honours its entry cap");
  fs::remove_all(base, ec);
}

void test_browse_anywhere() {
  const fs::path base =
      fs::temp_directory_path() / "molink-browse-anywhere-test";
  const fs::path home = base / "home";
  const fs::path outside = base / "outside";
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(home, ec);
  fs::create_directories(outside, ec);
  {
    std::ofstream file(outside / "note.txt", std::ios::binary);
    file << "outside the home folder";
  }
  set_home(native(home));

  std::vector<std::string> json;
  molink::file_transfer transfer(
      [&json](std::string value) { json.push_back(std::move(value)); },
      [](uint32_t, uint64_t, const uint8_t*, std::size_t) { return true; });

  transfer.browse("");
  expect(!json.empty(), "browse with an empty path answers");
  if (!json.empty()) {
    const uvcpp_json answer = uvcpp_json::parse(json.back());
    expect(answer.value("action", std::string()) == "listing" &&
               answer.value("ok", false),
           "roots view is a listing");
    expect(answer.value("path", std::string("x")).empty(),
           "roots view has an empty path");
    expect(answer.contains("roots") && answer["roots"].size() >= 1,
           "roots view carries the volume list");
    expect(answer.contains("entries") && answer["entries"].size() >= 1,
           "roots view carries clickable root rows");
  }

  transfer.browse(native(outside));
  const uvcpp_json listing = uvcpp_json::parse(json.back());
  expect(listing.value("ok", false), "an absolute path outside home lists");
  expect(listing.value("path", std::string()) == native(outside),
         "listing echoes the absolute path");
  bool found = false;
  if (listing.contains("entries")) {
    for (const auto& entry : listing["entries"]) {
      if (entry.value("name", std::string()) == "note.txt") found = true;
    }
  }
  expect(found, "the outside file appears in the listing");

  transfer.request(native(outside / "note.txt"));
  const uvcpp_json sending = uvcpp_json::parse(json.back());
  expect(sending.value("action", std::string()) == "sending" &&
             sending.value("ok", false),
         "a download outside home is allowed");

  transfer.browse(native(outside / "note.txt"));
  const uvcpp_json file_answer = uvcpp_json::parse(json.back());
  expect(file_answer.value("ok", true) == false &&
             file_answer.value("message", std::string()) == "not a folder",
         "browsing a file reports not a folder");

  transfer.browse(native(base / "no-such-folder"));
  const uvcpp_json missing = uvcpp_json::parse(json.back());
  expect(missing.value("ok", true) == false &&
             missing.value("message", std::string()) == "no such folder",
         "browsing a missing folder reports a readable error");

  fs::remove_all(base, ec);
}

}  // namespace

int main() {
  test_posix_normalization();
  test_windows_normalization();
  test_roots_from_drives();
  test_list_and_walk();
  test_browse_anywhere();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
