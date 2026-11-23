#include "clipboard/clipboard_files.h"

#include <utility>

namespace molink {

clipboard_path_build build_clipboard_files(
    const std::vector<clipboard_path_entry>& entries) {
  clipboard_path_build built;
  built.files.reserve(entries.size());
  for (const clipboard_path_entry& entry : entries) {
    if (entry.path.empty() || entry.name.empty()) {
      ++built.invalid;
      continue;
    }
    clipboard_file file;
    file.name = entry.name;
    file.path = entry.path;
    file.size = entry.size;
    // Deliberately no stream_id: a path-backed entry is served by
    // read_clipboard_pull() straight from disk, one requested range at a
    // time. The viewer's FUSE mount still pulls lazily.
    built.files.push_back(std::move(file));
  }
  return built;
}

}  // namespace molink
