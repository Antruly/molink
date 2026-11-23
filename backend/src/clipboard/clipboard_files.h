#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "clipboard/clipboard.h"

namespace molink {

/// One local file a platform clipboard enumerated from a path list
/// (CF_HDROP on Windows, text/uri-list on X11).
///
/// Platform backends do the filesystem work -- attribute lookup, directory
/// expansion, size -- and hand the result here. Keeping the mapping in this
/// file means the Windows drag-and-drop path can be covered by a test that
/// runs on every OS.
struct clipboard_path_entry {
  std::string path;
  std::string name;
  uint64_t size = 0;
};

/// Hard stop for one platform enumeration. A directory copy can name an
/// unbounded number of files; the walk stops here and the backend logs a
/// warning. The session layer then applies the advertised 8-file / 1 TiB
/// caps and reports its own `omitted` count to the viewer.
constexpr std::size_t kClipboardPathScanMax = 64;

struct clipboard_path_build {
  std::vector<clipboard_file> files;
  /// Entries with an empty path or name. They cannot be announced and are
  /// counted here instead of being dropped silently.
  std::size_t invalid = 0;
};

/// Converts enumerated local files into the neutral list the session layer
/// publishes. `stream_id` stays 0: these bytes live at `path` and are read
/// with ordinary ranged file I/O, so the host never materialises the whole
/// file and the viewer pulls only what it pastes.
clipboard_path_build build_clipboard_files(
    const std::vector<clipboard_path_entry>& entries);

}  // namespace molink
