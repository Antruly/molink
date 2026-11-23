// CF_HDROP enumeration -> neutral clipboard_file list.
//
// The drag-and-drop API itself is Windows-only, but the mapping (path, name,
// size, no stream token) and the invalid-entry handling are platform-neutral,
// so they are covered on every CI OS.

#include <cstdio>
#include <string>
#include <vector>

#include "clipboard/clipboard_files.h"

namespace {

using molink::build_clipboard_files;
using molink::clipboard_path_entry;
using molink::kClipboardPathScanMax;

int checks = 0;
int failures = 0;

void expect(bool value, const char* message) {
  ++checks;
  if (!value) {
    ++failures;
    std::printf("FAIL: %s\n", message);
  }
}

void test_maps_paths_to_files() {
  const std::vector<clipboard_path_entry> entries = {
      {"C:\\Users\\Administrator\\clip-test-200mb.bin",
       "clip-test-200mb.bin", 209715200ull},
      {"C:\\Temp\\notes.txt", "notes.txt", 0},
  };
  const molink::clipboard_path_build built = build_clipboard_files(entries);
  expect(built.files.size() == 2, "each path becomes one file");
  expect(built.invalid == 0, "valid entries are not reported invalid");
  expect(built.files[0].path == entries[0].path,
         "the local path is carried through unchanged");
  expect(built.files[0].name == entries[0].name,
         "the display name is carried through");
  expect(built.files[0].size == 209715200ull, "the size is carried through");
  // A real path is read with ranged file I/O. It must not be mistaken for an
  // OLE virtual file: a non-zero token would make the host ask the data
  // object for bytes it does not own.
  expect(built.files[0].stream_id == 0, "a path entry has no stream token");
  expect(built.files[1].size == 0, "an empty file keeps size 0");
}

void test_skips_entries_without_path_or_name() {
  const std::vector<clipboard_path_entry> entries = {
      {"", "nameless-path", 10},
      {"C:\\Temp\\ok.txt", "", 10},
      {"", "", 10},
      {"C:\\Temp\\ok.txt", "ok.txt", 10},
  };
  const molink::clipboard_path_build built = build_clipboard_files(entries);
  expect(built.files.size() == 1, "only the complete entry is announced");
  expect(built.invalid == 3, "unusable entries are counted, not dropped");
  expect(built.files[0].name == "ok.txt", "the usable entry survives");
}

void test_keeps_everything_the_backend_enumerated() {
  // The backend walk stops at kClipboardPathScanMax; this mapper must not
  // apply the advertised 8-file / 1 TiB pull caps, because the session layer
  // needs the full list to compute and report its `omitted` count.
  std::vector<clipboard_path_entry> entries;
  for (std::size_t i = 0; i < kClipboardPathScanMax + 2; ++i) {
    entries.push_back({"C:\\Temp\\f" + std::to_string(i) + ".bin",
                       "f" + std::to_string(i) + ".bin", 1});
  }
  const molink::clipboard_path_build built = build_clipboard_files(entries);
  expect(built.files.size() == entries.size(),
         "the mapper does not apply the session-layer caps");

  const std::vector<clipboard_path_entry> oversized = {
      {"D:\\huge.bin", "huge.bin", (1ull << 40) + 1}};
  const molink::clipboard_path_build large =
      build_clipboard_files(oversized);
  expect(large.files.size() == 1,
         "an oversized file is still reported so the session can count it");
  expect(large.files[0].size == (1ull << 40) + 1,
         "the oversized size is preserved");
}

}  // namespace

int main() {
  test_maps_paths_to_files();
  test_skips_entries_without_path_or_name();
  test_keeps_everything_the_backend_enumerated();
  std::printf("%d checks, %d failure(s)\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
