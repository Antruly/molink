#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "clipboard/clipboard.h"

namespace molink {

/// One local file or directory a platform clipboard enumerated from a path
/// list (CF_HDROP on Windows, text/uri-list on X11).
///
/// Platform backends hand the raw roots here; the walk lives in
/// clipboard_files.cpp so it is covered by a test that runs on every OS.
struct clipboard_path_entry {
  std::string path;
  /// Last path component; never a relative path.
  std::string name;
  uint64_t size = 0;
  bool is_dir = false;
  /// Selection-relative path, `/`-separated. Empty means `name`.
  std::string rel;
};

/// Hard stop for one platform enumeration. A directory copy can name an
/// unbounded number of files; the walk stops storing entries here and the
/// backend logs `clipboard.files truncated=1 omitted=N`. The session layer
/// still batches at kClipboardPullMaxFiles when it announces the list.
constexpr std::size_t kClipboardPathScanMax = 2000;

/// Deepest directory chain the walk follows. A junction/symlink loop must not
/// turn one clipboard copy into an endless walk; symlinks themselves are never
/// followed.
constexpr unsigned kClipboardPathScanMaxDepth = 8;

/// Result of walking a path list.
struct clipboard_scan_result {
  std::vector<clipboard_path_entry> entries;
  std::size_t directories = 0;
  std::size_t empty_directories = 0;
  std::size_t unreadable = 0;
  /// Entries found after kClipboardPathScanMax was reached. They are counted
  /// but not retained, so the caller can report how much was left out.
  std::size_t omitted = 0;
  bool truncated = false;
};

/// Walks @p roots (files or directories) and returns every entry in the
/// selection, directories included, with a `/`-separated relative path.
/// Duplicate paths are announced once. Directories are never followed when
/// they are symlinks/reparse points.
clipboard_scan_result scan_clipboard_paths(
    const std::vector<std::string>& roots,
    std::size_t max_entries = kClipboardPathScanMax);

/// Joins a parent relative path and a child name with `/`. Used by the walk and
/// exposed so the wire format is covered without a filesystem.
std::string clipboard_relative_path(const std::string& parent,
                                    const std::string& child);

struct clipboard_path_build {
  std::vector<clipboard_file> files;
  /// Entries with an empty path or name. They cannot be announced and are
  /// counted here instead of being dropped silently.
  std::size_t invalid = 0;
};

/// Converts enumerated local entries into the neutral list the session layer
/// publishes. `stream_id` stays 0: these bytes live at `path` and are read
/// with ordinary ranged file I/O, so the host never materialises the whole
/// file and the viewer pulls only what it pastes.
clipboard_path_build build_clipboard_files(
    const std::vector<clipboard_path_entry>& entries);

}  // namespace molink
