#include "clipboard/clipboard_files.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>

#include "util.h"

namespace molink {
namespace {

namespace fs = std::filesystem;

struct walk_state {
  clipboard_scan_result* result = nullptr;
  std::size_t max_entries = kClipboardPathScanMax;
  std::set<std::string> seen_paths;
};

bool store_entry(walk_state& state, clipboard_path_entry entry) {
  if (state.result->entries.size() >= state.max_entries) {
    state.result->truncated = true;
    ++state.result->omitted;
    return false;
  }
  state.result->entries.push_back(std::move(entry));
  return true;
}

void walk_directory(const fs::path& directory, const std::string& rel_parent,
                    unsigned depth, walk_state& state);

/// Adds one filesystem object. Directories are kept as entries (including an
/// empty one) and then walked; regular files are kept; symlinks and other
/// special files are skipped.
void add_item(const fs::path& path, const std::string& rel_parent,
              unsigned depth, walk_state& state) {
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(path, ec);
  if (ec) {
    ++state.result->unreadable;
    return;
  }
  if (fs::is_symlink(status)) return;
  const std::string name = path_to_utf8(path.filename());
  if (name.empty()) {
    ++state.result->unreadable;
    return;
  }
  const std::string rel = clipboard_relative_path(rel_parent, name);

  if (fs::is_directory(status)) {
    ++state.result->directories;
    clipboard_path_entry entry;
    entry.path = path_to_utf8(path);
    entry.name = name;
    entry.is_dir = true;
    entry.rel = rel;
    if (entry.path.empty()) {
      ++state.result->unreadable;
      return;
    }
    // "Empty" means the walk below stored nothing. Measure after this
    // directory's own entry so an empty directory is still counted as one.
    const std::string key = path_to_utf8(path);
    if (state.seen_paths.insert(key).second) {
      store_entry(state, std::move(entry));
    }
    const std::size_t before_children = state.result->entries.size();
    walk_directory(path, rel, depth + 1, state);
    if (state.result->entries.size() == before_children) {
      ++state.result->empty_directories;
    }
    return;
  }
  if (!fs::is_regular_file(status)) return;

  clipboard_path_entry entry;
  entry.path = path_to_utf8(path);
  entry.name = name;
  entry.rel = rel;
  entry.size = static_cast<uint64_t>(fs::file_size(path, ec));
  if (ec) {
    entry.size = 0;
    ++state.result->unreadable;
  }
  if (entry.path.empty()) {
    ++state.result->unreadable;
    return;
  }
  const std::string key = entry.path;
  if (state.seen_paths.insert(key).second) {
    store_entry(state, std::move(entry));
  }
}

void walk_directory(const fs::path& directory, const std::string& rel_parent,
                    unsigned depth, walk_state& state) {
  if (depth > kClipboardPathScanMaxDepth) {
    state.result->truncated = true;
    return;
  }
  std::error_code ec;
  fs::directory_iterator iterator(directory, ec);
  if (ec) {
    ++state.result->unreadable;
    return;
  }
  // Directory order is filesystem-defined. Sorting makes the announced list
  // (and therefore the batches) deterministic for a given tree.
  std::vector<fs::path> children;
  for (const fs::directory_entry& item : iterator) {
    children.push_back(item.path());
  }
  std::sort(children.begin(), children.end(),
            [](const fs::path& a, const fs::path& b) {
              return a.filename().generic_string() <
                     b.filename().generic_string();
            });
  for (const fs::path& child : children) {
    add_item(child, rel_parent, depth, state);
  }
}

}  // namespace

std::string clipboard_relative_path(const std::string& parent,
                                    const std::string& child) {
  if (parent.empty()) return child;
  if (child.empty()) return parent;
  std::string out = parent;
  if (out.back() != '/') out.push_back('/');
  out += child;
  return out;
}

clipboard_scan_result scan_clipboard_paths(const std::vector<std::string>& roots,
                                           std::size_t max_entries) {
  clipboard_scan_result result;
  walk_state state;
  state.result = &result;
  state.max_entries = max_entries == 0 ? kClipboardPathScanMax : max_entries;
  for (const std::string& root : roots) {
    if (root.empty()) {
      ++result.unreadable;
      continue;
    }
    const fs::path path = path_from_utf8(root);
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec) {
      ++result.unreadable;
      continue;
    }
    if (fs::is_symlink(status)) continue;
    const std::string name = path_to_utf8(path.filename());
    if (name.empty()) {
      ++result.unreadable;
      continue;
    }
    add_item(path, std::string(), 0, state);
  }
  return result;
}

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
    file.is_dir = entry.is_dir;
    file.rel = entry.rel.empty() ? entry.name : entry.rel;
    // Deliberately no stream_id: a path-backed entry is served by
    // read_clipboard_pull() straight from disk, one requested range at a
    // time. The viewer's FUSE mount still pulls lazily.
    built.files.push_back(std::move(file));
  }
  return built;
}

}  // namespace molink
