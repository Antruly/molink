// The native file panel's path grammar, multi-selection and task aggregation
// are Win32-free copies of the Web's file-transfer.ts. Pinning them here means
// the Direct2D client and the page make the same decisions without a display.

#include <cstdio>
#include <string>
#include <vector>

#include "file_panel_logic.h"

namespace {

using namespace molink::files;

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

void expect_int(int actual, int expected, const char* description) {
  ++checks;
  if (actual == expected) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n  expected: %d\n  actual:   %d\n",
               description, expected, actual);
}

void test_normalize_remote_path() {
  expect_eq(normalize_remote_path("/home/me/../you/./docs"), "/home/you/docs",
            "POSIX path collapses . and ..");
  expect_eq(normalize_remote_path("C:\\Users\\me\\..\\you"),
            "C:/Users/you", "drive path uses forward slashes");
  expect_eq(normalize_remote_path("//server/share/./x"),
            "//server/share/x", "UNC prefix survives");
  expect_eq(normalize_remote_path("/../../etc"), "/etc",
            ".. cannot climb above the root");
  expect_eq(normalize_remote_path("D:\\"), "D:/", "drive root keeps its slash");
}

void test_normalize_relative_path() {
  expect_eq(normalize_relative_path("a\\b//c/"), "a/b/c",
            "relative path uses slashes and drops duplicates");
  expect_eq(normalize_relative_path("././x"), "x",
            "leading ./ prefixes are removed");
  expect_eq(normalize_relative_path("..\\pics/"), "../pics",
            ".. stays a relative segment");
}

void test_resolve_remote_path() {
  expect_eq(resolve_remote_path("docs", "/home/me", ""), "/home/me/docs",
            "relative path resolves against the display folder");
  expect_eq(resolve_remote_path("..", "/home/me/docs", ""), "/home/me",
            ".. pops one folder");
  expect_eq(resolve_remote_path("../pics", "/home/me/docs", ""),
            "/home/me/pics", ".. plus a name");
  expect_eq(resolve_remote_path("/srv/data", "/home/me", ""), "/srv/data",
            "absolute POSIX path ignores the base");
  expect_eq(resolve_remote_path("D:\\work", "/home/me", ""), "D:/work",
            "absolute drive path ignores the base");
  expect_eq(resolve_remote_path("~", "/home/me", "/home/me"), "/home/me",
            "~ expands to the host home");
  expect_eq(resolve_remote_path("~/Documents", "/home/me", "/home/me"),
            "/home/me/Documents", "~/x expands to the host home");
  expect_eq(resolve_remote_path("~/Documents", "", ""), "Documents",
            "without a known home ~/x stays home-relative");
  expect_eq(resolve_remote_path("", "/home/me", "/home/me"), "/home/me",
            "empty input opens the host home");
  expect_eq(resolve_remote_path("   ", "/home/me", "/home/me"), "/home/me",
            "whitespace counts as empty");
  expect_eq(resolve_remote_path("C:\\Windows", "D:\\data", "C:\\Users\\me"),
            "C:/Windows", "typing C:\\Windows jumps to the drive");
}

void test_join_and_parent() {
  expect_eq(join_path("C:\\", "Windows"), "C:\\Windows",
            "join keeps a backslash base");
  expect_eq(join_path("C:/Users", "me"), "C:/Users/me",
            "join keeps a slash base");
  expect_eq(join_path("/", "etc"), "/etc", "join from POSIX root");
  expect_eq(join_path("", "x"), "x", "join with no base");

  expect_eq(parent_path("/etc"), "/", "POSIX parent");
  expect_eq(parent_path("/etc/ssh"), "/etc", "POSIX nested parent");
  expect_eq(parent_path("/"), "", "POSIX root has no parent");
  expect_eq(parent_path("C:\\Windows"), "C:\\", "drive child parent");
  expect_eq(parent_path("C:\\Windows\\System32"), "C:\\Windows",
            "drive nested parent");
  expect_eq(parent_path("C:\\"), "", "drive root has no parent");
  expect_eq(parent_path("\\\\server\\share"), "",
            "UNC share root has no parent");
  expect_eq(parent_path("\\\\server\\share\\dir"), "\\\\server\\share",
            "UNC child parent");
}

void test_path_crumbs() {
  const std::vector<path_crumb> posix = path_crumbs("/var/log");
  expect_int(static_cast<int>(posix.size()), 3, "POSIX crumbs: count");
  if (posix.size() == 3) {
    expect_eq(posix[0].name, "/", "POSIX crumb 0 name");
    expect_eq(posix[0].path, "/", "POSIX crumb 0 path");
    expect_eq(posix[1].name, "var", "POSIX crumb 1 name");
    expect_eq(posix[1].path, "/var", "POSIX crumb 1 path");
    expect_eq(posix[2].name, "log", "POSIX crumb 2 name");
    expect_eq(posix[2].path, "/var/log", "POSIX crumb 2 path");
  }

  const std::vector<path_crumb> drive = path_crumbs("C:\\Users\\me");
  expect_int(static_cast<int>(drive.size()), 3, "drive crumbs: count");
  if (drive.size() == 3) {
    expect_eq(drive[0].name, "C:", "drive crumb 0 name");
    expect_eq(drive[0].path, "C:\\", "drive crumb 0 path");
    expect_eq(drive[1].name, "Users", "drive crumb 1 name");
    expect_eq(drive[1].path, "C:\\Users", "drive crumb 1 path");
    expect_eq(drive[2].name, "me", "drive crumb 2 name");
    expect_eq(drive[2].path, "C:\\Users\\me", "drive crumb 2 path");
  }

  const std::vector<path_crumb> unc =
      path_crumbs("\\\\server\\share\\dir");
  expect_int(static_cast<int>(unc.size()), 2, "UNC crumbs: count");
  if (unc.size() == 2) {
    expect_eq(unc[0].name, "\\\\server\\share", "UNC share crumb name");
    expect_eq(unc[1].path, "\\\\server\\share\\dir", "UNC child crumb path");
  }
  expect(path_crumbs("").empty(), "roots view has no crumbs");
}

void test_path_flavor() {
  expect(path_flavor("C:\\Windows") == path_flavor_kind::windows,
         "drive path is Windows");
  expect(path_flavor("C:/Windows") == path_flavor_kind::windows,
         "drive path with slash is Windows");
  expect(path_flavor("\\\\server\\share") == path_flavor_kind::windows,
         "UNC path is Windows");
  expect(path_flavor("/var/log") == path_flavor_kind::posix,
         "POSIX path is POSIX");
  expect(path_flavor("relative") == path_flavor_kind::unknown,
         "bare name has no flavor");
  expect(path_flavor("") == path_flavor_kind::unknown,
         "empty path has no flavor");
}

void test_selection() {
  selection_model sel;
  sel.click(2, false, false, 6);
  expect_int(static_cast<int>(sel.size()), 1, "plain click selects one");
  expect(sel.contains(2), "plain click selects the row");

  sel.click(4, true, false, 6);
  expect_int(static_cast<int>(sel.size()), 2, "ctrl click adds one");
  expect(sel.contains(2) && sel.contains(4), "ctrl click keeps the old row");

  sel.click(4, true, false, 6);
  expect(!sel.contains(4), "ctrl click toggles a selected row off");

  sel.click(2, false, false, 6);
  sel.click(1, false, false, 6);
  sel.click(4, false, true, 6);
  expect_int(static_cast<int>(sel.size()), 4,
             "shift without ctrl selects the range only");
  expect(sel.contains(1) && sel.contains(2) && sel.contains(3) &&
             sel.contains(4),
         "shift range covers every row between anchor and target");

  sel.clear();
  sel.click(5, false, false, 6);
  sel.click(1, true, true, 6);
  expect_int(static_cast<int>(sel.size()), 5,
             "ctrl+shift adds the range to the selection");

  sel.clear();
  sel.toggle(3);
  expect(sel.contains(3), "toggle selects");
  sel.toggle(3);
  expect(!sel.contains(3), "toggle deselects");

  sel.select_all(4);
  expect_int(static_cast<int>(sel.size()), 4, "select all fills the set");
  expect(sel.all_selected(4), "select all reports all selected");
  expect(!sel.all_selected(5), "select all does not match a larger count");
  sel.clear();
  expect(sel.empty() && !sel.all_selected(0),
         "cleared selection is empty and not all of nothing");

  sel.click(9, false, false, 4);
  expect(sel.empty(), "out-of-range click is ignored");
}

void test_task_summary() {
  std::vector<transfer_item> items;
  items.push_back({100, 100, transfer_status::done, false});
  items.push_back({50, 20, transfer_status::error, false});
  items.push_back({30, 0, transfer_status::cancelled, false});
  items.push_back({10, 5, transfer_status::running, false});
  items.push_back({0, 0, transfer_status::queued, true});
  const transfer_summary summary = summarize_transfers(items);
  expect_int(summary.total, 5, "summary counts skipped in the total");
  expect_int(summary.skipped, 1, "summary counts skipped");
  expect_int(summary.done_files, 1, "summary counts done files");
  expect_int(summary.failed_files, 1, "summary counts failed files");
  expect_int(summary.cancelled_files, 1, "summary counts cancelled files");
  expect_int(static_cast<int>(summary.total_bytes), 190,
             "summary sums the non-skipped sizes");
  expect_int(static_cast<int>(summary.done_bytes), 125,
             "summary uses partial bytes for failed/running rows");
  expect_int(summary.percent, 66, "summary rounds the percentage");
  expect(summary.status == transfer_status::running,
         "running child wins the task status");

  std::vector<transfer_item> finished;
  finished.push_back({100, 100, transfer_status::done, false});
  finished.push_back({20, 20, transfer_status::error, false});
  const transfer_summary failed = summarize_transfers(finished);
  expect(failed.status == transfer_status::error, "failed child marks error");
  expect_int(failed.percent, 100, "finished task reaches 100%");

  std::vector<transfer_item> cancelled;
  cancelled.push_back({10, 10, transfer_status::cancelled, false});
  cancelled.push_back({10, 10, transfer_status::error, false});
  expect(summarize_transfers(cancelled).status ==
             transfer_status::cancelled,
         "cancellation wins over individual failures");

  std::vector<transfer_item> empty_files;
  empty_files.push_back({0, 0, transfer_status::done, false});
  empty_files.push_back({0, 0, transfer_status::done, false});
  expect_int(summarize_transfers(empty_files).percent, 100,
             "zero-byte finished task reaches 100%");

  std::vector<transfer_item> queued;
  queued.push_back({0, 0, transfer_status::queued, false});
  expect_int(summarize_transfers(queued).percent, 0,
             "zero-byte queued task stays at 0%");

  std::vector<transfer_item> skipped_only;
  skipped_only.push_back({0, 0, transfer_status::queued, true});
  const transfer_summary skipped = summarize_transfers(skipped_only);
  expect_int(skipped.total, 1, "skipped-only task keeps its total");
  expect(skipped.status == transfer_status::done,
         "skipped-only task is finished");
}

void test_block_reasons() {
  expect(upload_block_reason(2, true, true, false) == transfer_block::none,
         "upload is allowed with a selection and a target");
  expect(upload_block_reason(0, true, true, false) ==
             transfer_block::no_selection,
         "upload needs a selection");
  expect(upload_block_reason(2, false, true, false) ==
             transfer_block::no_source,
         "upload needs a source");
  expect(upload_block_reason(2, true, false, false) ==
             transfer_block::no_target,
         "upload needs a remote target");
  expect(upload_block_reason(2, true, true, true) == transfer_block::busy,
         "busy outranks the other upload reasons");

  expect(download_block_reason(2, false) == transfer_block::none,
         "download is allowed with a selection");
  expect(download_block_reason(0, false) == transfer_block::no_selection,
         "download needs a selection");
  expect(download_block_reason(2, true) == transfer_block::busy,
         "busy outranks the selection for downloads");
}

}  // namespace

int main() {
  test_normalize_remote_path();
  test_resolve_remote_path();
  test_join_and_parent();
  test_path_crumbs();
  test_path_flavor();
  test_selection();
  test_task_summary();
  test_block_reasons();
  if (failures != 0) {
    std::fprintf(stderr, "%d/%d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("file_panel_logic: %d checks passed\n", checks);
  return 0;
}
