#include "file_panel_logic.h"

#include <algorithm>
#include <cctype>

namespace molink::files {
namespace {

bool is_separator(char c) { return c == '/' || c == '\\'; }

bool is_drive_letter(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

std::string trim(const std::string& text) {
  std::size_t first = 0;
  while (first < text.size() &&
         (text[first] == ' ' || text[first] == '\t' || text[first] == '\n' ||
          text[first] == '\r')) {
    ++first;
  }
  std::size_t last = text.size();
  while (last > first &&
         (text[last - 1] == ' ' || text[last - 1] == '\t' ||
          text[last - 1] == '\n' || text[last - 1] == '\r')) {
    --last;
  }
  return text.substr(first, last - first);
}

std::vector<std::string> split_segments(const std::string& text) {
  std::vector<std::string> out;
  std::string current;
  for (char c : text) {
    if (c == '/' || c == '\\') {
      if (!current.empty()) out.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) out.push_back(std::move(current));
  return out;
}

bool drive_prefix(const std::string& path, std::string* drive,
                  std::size_t* rest_at) {
  if (path.size() >= 2 && is_drive_letter(path[0]) && path[1] == ':') {
    *drive = path.substr(0, 2);
    *rest_at = 2;
    return true;
  }
  drive->clear();
  *rest_at = 0;
  return false;
}

}  // namespace

std::string normalize_relative_path(const std::string& raw) {
  std::string text;
  text.reserve(raw.size());
  for (char c : raw) text.push_back(c == '\\' ? '/' : c);
  // Strip any number of leading "./" prefixes.
  while (text.size() >= 2 && text[0] == '.' && text[1] == '/') {
    text.erase(0, 2);
  }
  std::string collapsed;
  collapsed.reserve(text.size());
  for (char c : text) {
    if (c == '/' && !collapsed.empty() && collapsed.back() == '/') continue;
    collapsed.push_back(c);
  }
  while (!collapsed.empty() && collapsed.back() == '/') collapsed.pop_back();
  return collapsed;
}

std::string normalize_remote_path(const std::string& raw) {
  std::string forward;
  forward.reserve(raw.size());
  for (char c : raw) forward.push_back(c == '\\' ? '/' : c);

  std::string drive;
  std::size_t rest_at = 0;
  drive_prefix(forward, &drive, &rest_at);
  const std::string rest = forward.substr(rest_at);
  const bool unc = rest.size() >= 2 && rest[0] == '/' && rest[1] == '/';
  const bool absolute = !rest.empty() && rest[0] == '/';

  std::vector<std::string> parts;
  std::size_t begin = 0;
  while (begin <= rest.size()) {
    const std::size_t slash = rest.find('/', begin);
    const std::size_t end =
        slash == std::string::npos ? rest.size() : slash;
    const std::string segment = rest.substr(begin, end - begin);
    if (!segment.empty() && segment != ".") {
      if (segment == "..") {
        if (!parts.empty() && parts.back() != "..") {
          parts.pop_back();
        } else if (!absolute && !unc) {
          parts.push_back("..");
        }
      } else {
        parts.push_back(segment);
      }
    }
    if (slash == std::string::npos) break;
    begin = slash + 1;
  }

  std::string out = drive;
  if (unc) {
    out += "//";
  } else if (absolute) {
    out += "/";
  }
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out.push_back('/');
    out += parts[i];
  }
  return out.empty() ? "." : out;
}

bool is_absolute_path(const std::string& path) {
  if (path.empty()) return false;
  if (path[0] == '/') return true;
  if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') return true;
  return path.size() >= 3 && is_drive_letter(path[0]) && path[1] == ':' &&
         is_separator(path[2]);
}

std::string resolve_remote_path(const std::string& text,
                                const std::string& base,
                                const std::string& root) {
  const std::string raw = trim(text);
  if (raw.empty()) return root;
  const std::string home = trim(root);
  if (raw == "~") return home;
  if (raw.size() >= 2 && raw[0] == '~' && is_separator(raw[1])) {
    const std::string rest = raw.substr(2);
    const std::string joined = home.empty() ? rest : home + "/" + rest;
    return normalize_remote_path(joined);
  }
  const bool absolute = is_absolute_path(raw);
  if (!absolute && base.empty()) return normalize_remote_path(raw);
  return normalize_remote_path(absolute ? raw : base + "/" + raw);
}

std::string join_path(const std::string& base, const std::string& name) {
  if (base.empty()) return name;
  const char separator = base.find('\\') != std::string::npos ? '\\' : '/';
  std::size_t end = base.size();
  while (end > 0 && is_separator(base[end - 1])) --end;
  if (end == 0) return std::string(1, separator) + name;
  return base.substr(0, end) + separator + name;
}

std::string parent_path(const std::string& path) {
  if (path.empty()) return std::string();
  const char separator = path.find('\\') != std::string::npos ? '\\' : '/';
  std::string text = path;
  while (!text.empty() && is_separator(text.back())) text.pop_back();
  if (text.empty()) return std::string();
  // UNC share root: \\server\share has no browsable parent.
  if (text.size() >= 2 && text[0] == '\\' && text[1] == '\\') {
    if (split_segments(text.substr(2)).size() <= 2) return std::string();
  }
  const std::size_t cut = text.find_last_of("/\\");
  if (cut == std::string::npos) return std::string();
  const std::string parent = text.substr(0, cut);
  if (parent.empty()) return std::string(1, separator);
  if (parent.size() == 2 && is_drive_letter(parent[0]) && parent[1] == ':') {
    return parent + separator;
  }
  return parent;
}

path_flavor_kind path_flavor(const std::string& path) {
  if (path.empty()) return path_flavor_kind::unknown;
  if (path.size() >= 3 && is_drive_letter(path[0]) && path[1] == ':' &&
      is_separator(path[2])) {
    return path_flavor_kind::windows;
  }
  if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
    return path_flavor_kind::windows;
  }
  if (path.find('\\') != std::string::npos) {
    return path_flavor_kind::windows;
  }
  if (path[0] == '/') return path_flavor_kind::posix;
  return path_flavor_kind::unknown;
}

std::vector<path_crumb> path_crumbs(const std::string& path) {
  std::vector<path_crumb> out;
  if (path.empty()) return out;
  const char separator =
      path.find('\\') != std::string::npos ? '\\' : '/';
  const path_flavor_kind flavor = path_flavor(path);
  if (flavor == path_flavor_kind::windows && path.size() >= 2 &&
      (path[0] == '\\' || path[0] == '/') &&
      (path[1] == '\\' || path[1] == '/')) {
    const std::vector<std::string> parts = split_segments(path);
    if (parts.size() < 2) return out;
    std::string current = "\\\\" + parts[0] + "\\" + parts[1];
    out.push_back({current, current});
    for (std::size_t i = 2; i < parts.size(); ++i) {
      current += separator;
      current += parts[i];
      out.push_back({parts[i], current});
    }
    return out;
  }
  if (flavor == path_flavor_kind::windows) {
    std::string drive;
    std::size_t rest_at = 0;
    if (!drive_prefix(path, &drive, &rest_at)) return out;
    const std::string root = drive + separator;
    out.push_back({drive, root});
    std::string current = root;
    for (const std::string& part : split_segments(path.substr(rest_at))) {
      current += part;
      out.push_back({part, current});
      current += separator;
    }
    return out;
  }
  if (path[0] == '/') {
    out.push_back({"/", "/"});
    std::string current;
    for (const std::string& part : split_segments(path)) {
      current += "/";
      current += part;
      out.push_back({part, current});
    }
    return out;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void selection_model::click(std::size_t index, bool ctrl, bool shift,
                            std::size_t count) {
  if (count == 0 || index >= count) return;
  if (shift && has_anchor_) {
    if (!ctrl) selected_.clear();
    std::size_t first = anchor_;
    std::size_t last = index;
    if (first > last) std::swap(first, last);
    if (last >= count) last = count > 0 ? count - 1 : 0;
    for (std::size_t i = first; i <= last; ++i) selected_.insert(i);
    return;
  }
  if (ctrl) {
    toggle(index);
    return;
  }
  selected_.clear();
  selected_.insert(index);
  anchor_ = index;
  has_anchor_ = true;
}

void selection_model::toggle(std::size_t index) {
  const auto it = selected_.find(index);
  if (it == selected_.end()) {
    selected_.insert(index);
  } else {
    selected_.erase(it);
  }
  anchor_ = index;
  has_anchor_ = true;
}

void selection_model::set(std::size_t index, bool selected) {
  if (selected) {
    selected_.insert(index);
  } else {
    selected_.erase(index);
  }
  anchor_ = index;
  has_anchor_ = true;
}

bool selection_model::contains(std::size_t index) const {
  return selected_.find(index) != selected_.end();
}

void selection_model::select_all(std::size_t count) {
  selected_.clear();
  for (std::size_t i = 0; i < count; ++i) selected_.insert(i);
  anchor_ = 0;
  has_anchor_ = count > 0;
}

void selection_model::clear() {
  selected_.clear();
  anchor_ = 0;
  has_anchor_ = false;
}

// ---------------------------------------------------------------------------
// Task aggregation
// ---------------------------------------------------------------------------

transfer_summary summarize_transfers(const std::vector<transfer_item>& items) {
  transfer_summary out;
  int children = 0;
  bool running = false;
  bool cancelled = false;
  bool failed = false;
  for (const transfer_item& item : items) {
    if (item.skipped) {
      ++out.skipped;
      continue;
    }
    ++children;
    if (item.size > 0 && out.total_bytes <= UINT64_MAX - item.size) {
      out.total_bytes += item.size;
    }
    const uint64_t progress = (std::min)(item.progress, item.size);
    switch (item.status) {
      case transfer_status::done:
        ++out.done_files;
        out.done_bytes += item.size;
        break;
      case transfer_status::error:
        ++out.failed_files;
        out.done_bytes += progress;
        failed = true;
        break;
      case transfer_status::cancelled:
        ++out.cancelled_files;
        cancelled = true;
        break;
      case transfer_status::queued:
      case transfer_status::running:
        out.done_bytes += progress;
        running = true;
        break;
    }
  }
  out.total = children + out.skipped;
  if (out.total_bytes == 0) {
    const bool all_finished =
        children > 0 &&
        out.done_files + out.failed_files + out.cancelled_files == children;
    out.percent = all_finished ? 100 : 0;
  } else {
    const uint64_t scaled =
        (out.done_bytes * 100 + out.total_bytes / 2) / out.total_bytes;
    out.percent = static_cast<int>((std::min)(uint64_t{100}, scaled));
  }
  if (running) {
    out.status = transfer_status::running;
  } else if (cancelled) {
    out.status = transfer_status::cancelled;
  } else if (failed) {
    out.status = transfer_status::error;
  } else {
    out.status = transfer_status::done;
  }
  return out;
}

transfer_block upload_block_reason(std::size_t selected, bool has_source,
                                   bool has_target, bool busy) {
  if (busy) return transfer_block::busy;
  if (selected == 0) return transfer_block::no_selection;
  if (!has_source) return transfer_block::no_source;
  if (!has_target) return transfer_block::no_target;
  return transfer_block::none;
}

transfer_block download_block_reason(std::size_t selected, bool busy) {
  if (busy) return transfer_block::busy;
  if (selected == 0) return transfer_block::no_selection;
  return transfer_block::none;
}

}  // namespace molink::files
