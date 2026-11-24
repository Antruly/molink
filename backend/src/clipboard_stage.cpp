#include "clipboard_stage.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <system_error>
#include <utility>

#include "util.h"

namespace molink {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kOrphanSweepIntervalMs = 60ull * 1000;

std::string random_stage_id() {
  static const char kHex[] = "0123456789abcdef";
  std::random_device device;
  std::string id;
  id.reserve(24);
  for (int i = 0; i < 12; ++i) {
    const unsigned byte = static_cast<unsigned>(device() & 0xffu);
    id.push_back(kHex[byte >> 4]);
    id.push_back(kHex[byte & 0x0fu]);
  }
  return id;
}

/// Windows compares file names case-insensitively, so two staged names that
/// differ only by case must still get distinct final paths.
bool same_name(const std::string& a, const std::string& b) {
#if defined(_WIN32)
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    unsigned char cb = static_cast<unsigned char>(b[i]);
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
#else
  return a == b;
#endif
}

std::string trim_edges(std::string text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '.')) {
    text.erase(text.begin());
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '.')) {
    text.pop_back();
  }
  return text;
}

/// Splits a selection-relative path into safe components. Both separators are
/// accepted on the wire. Absolute paths, drive letters, empty/`.`/`..`
/// components and over-deep chains are rejected before any filesystem call.
bool sanitize_relative_path(const std::string& raw,
                            std::vector<std::string>& segments) {
  segments.clear();
  if (raw.empty() || raw.size() > 4096) return false;
  std::string text = raw;
  for (char& c : text) {
    if (c == '\\') c = '/';
  }
  if (text.front() == '/') return false;
  if (text.size() >= 2 &&
      std::isalpha(static_cast<unsigned char>(text[0])) && text[1] == ':') {
    return false;
  }
  while (!text.empty() && text.back() == '/') text.pop_back();
  if (text.empty()) return false;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('/', start);
    const std::string part =
        text.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
    if (part.empty() || part == "." || part == "..") return false;
    const std::string clean = clipboard_stage::sanitize_name(part);
    if (clean.empty() || clean == "." || clean == "..") return false;
    segments.push_back(clean);
    if (segments.size() > 64) return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return !segments.empty();
}

std::string join_relative_path(const std::vector<std::string>& segments) {
  std::string out;
  for (const std::string& segment : segments) {
    if (!out.empty()) out.push_back('/');
    out += segment;
  }
  return out;
}

/// True when @p rel names a top-level item (no parent) or when no announced
/// directory owns its parent. Such entries are the selection roots the OS
/// clipboard must reference; passing every descendant would paste duplicates.
bool is_selection_root(const std::string& rel,
                       const std::set<std::string>& directories) {
  const std::size_t slash = rel.rfind('/');
  if (slash == std::string::npos) return true;
  return directories.find(rel.substr(0, slash)) == directories.end();
}

}  // namespace

clipboard_stage::clipboard_stage(std::string directory, files_applier applier,
                                 clock_fn clock)
    : directory_(std::move(directory)),
      applier_(std::move(applier)),
      clock_(std::move(clock)) {}

std::string clipboard_stage::sanitize_name(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (const unsigned char c : raw) {
    if (c == 0) return std::string();
    // Path separators and drive/stream separators would let a crafted remote
    // name choose the destination. Reject instead of silently stripping them
    // so a caller can report a bad request.
    if (c == '/' || c == '\\' || c == ':') return std::string();
    if (c < 0x20 || c == 0x7f) continue;
    if (c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      continue;
    }
    out.push_back(static_cast<char>(c));
  }
  out = trim_edges(std::move(out));
  if (out.empty() || out == "." || out == "..") return std::string();
  if (out.size() > 180) {
    std::size_t cut = 180;
    // Do not split a UTF-8 sequence in half.
    while (cut > 0 &&
           (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) {
      --cut;
    }
    out.resize(cut);
    out = trim_edges(std::move(out));
    if (out.empty()) return std::string();
  }
  return out;
}

uint64_t clipboard_stage::now_ms() const {
  if (clock_) return clock_();
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

bool clipboard_stage::ensure_directory_locked() {
  if (directory_.empty()) return false;
  std::error_code ec;
  const fs::path directory = path_from_utf8(directory_);
  fs::create_directories(directory, ec);
  if (ec) return false;
  return fs::is_directory(directory, ec) && !ec;
}

bool clipboard_stage::valid_batch(const std::string& batch) {
  if (batch.size() < 8 || batch.size() > 64) return false;
  for (const char c : batch) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::string clipboard_stage::unique_final_path_locked(
    const std::string& name, const std::string& directory) const {
  const fs::path dir = path_from_utf8(directory);
  auto taken = [this](const fs::path& candidate) {
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec) return true;
    const std::string text = path_to_utf8(candidate.filename());
    for (const auto& item : entries_) {
      const fs::path existing = path_from_utf8(item.second.final_path);
      // Names only collide inside one directory; `a.txt` in another batch's
      // folder is a different file.
      if (existing.parent_path() == candidate.parent_path() &&
          same_name(path_to_utf8(existing.filename()), text)) {
        return true;
      }
    }
    return false;
  };

  const fs::path name_path = path_from_utf8(name);
  const fs::path relative_parent = name_path.parent_path();
  fs::path candidate = dir / name_path;
  if (!taken(candidate)) return path_to_utf8(candidate);
  const std::string stem = path_to_utf8(name_path.stem());
  const std::string extension = path_to_utf8(name_path.extension());
  for (int i = 1; i < 10000; ++i) {
    candidate = dir / relative_parent /
                path_from_utf8(stem + " (" + std::to_string(i) + ")" +
                               extension);
    if (!taken(candidate)) return path_to_utf8(candidate);
  }
  candidate = dir / relative_parent /
              path_from_utf8(stem + "-" + std::to_string(now_ms()) +
                             extension);
  return path_to_utf8(candidate);
}

void clipboard_stage::sweep_locked(uint64_t now) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (now > it->second.updated_ms &&
        now - it->second.updated_ms >= kEntryTtlMs) {
      std::error_code ec;
      if (it->second.is_directory) {
        // Best effort: only an empty directory is removed.
        fs::remove(path_from_utf8(it->second.final_path), ec);
      } else if (it->second.temp_path != it->second.final_path) {
        fs::remove(path_from_utf8(it->second.temp_path), ec);
      }
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  sweep_orphan_files_locked(now);
}

/// Leftovers from a previous process (or an apply that never happened) are
/// hidden `.stage-*` files. Remove ones older than the same TTL. Applied
/// files are kept for kAppliedTtlMs because the OS clipboard still references
/// them, then swept as well.
void clipboard_stage::sweep_orphan_files_locked(uint64_t now) {
  if (directory_.empty()) return;
  if (now >= last_orphan_sweep_ms_ &&
      now - last_orphan_sweep_ms_ < kOrphanSweepIntervalMs) {
    return;
  }
  last_orphan_sweep_ms_ = now;
  std::error_code ec;
  const auto file_now = fs::file_time_type::clock::now();
  auto sweep_one = [&](const fs::directory_entry& entry) {
    std::error_code file_ec;
    if (!entry.is_regular_file(file_ec) || file_ec) return;
    const std::string name = path_to_utf8(entry.path().filename());
    std::error_code time_ec;
    const auto written = entry.last_write_time(time_ec);
    if (time_ec) return;
    const bool pending = name.compare(0, 7, ".stage-") == 0;
    const uint64_t ttl = pending ? kEntryTtlMs : kAppliedTtlMs;
    if (written + std::chrono::milliseconds(ttl) < file_now) {
      fs::remove(entry.path(), time_ec);
    }
  };
  fs::directory_iterator it(path_from_utf8(directory_), ec);
  if (ec) return;
  for (const fs::directory_entry& entry : it) {
    std::error_code type_ec;
    if (entry.is_directory(type_ec) && !type_ec) {
      // Per-batch subdirectories (`<sha1>/`).
      std::error_code child_ec;
      fs::directory_iterator child(entry.path(), child_ec);
      if (child_ec) continue;
      for (const fs::directory_entry& item : child) sweep_one(item);
      continue;
    }
    sweep_one(entry);
  }
}

stage_error clipboard_stage::begin(const std::string& name, uint64_t size,
                                   std::string& id, std::string& path,
                                   const std::string& batch,
                                   const std::string& rel, bool is_dir) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t now = now_ms();
  sweep_locked(now);
  if (directory_.empty()) return stage_error::unavailable;
  if (is_dir) {
    if (size != 0) return stage_error::bad_request;
  } else if (size > kMaxFileBytes) {
    return stage_error::too_large;
  }
  if (!batch.empty() && !valid_batch(batch)) return stage_error::bad_request;
  const std::string clean = sanitize_name(name);
  if (clean.empty()) return stage_error::bad_request;
  std::vector<std::string> segments;
  if (!sanitize_relative_path(rel.empty() ? clean : rel, segments)) {
    return stage_error::bad_request;
  }
  const std::string relative = join_relative_path(segments);
  if (!ensure_directory_locked()) return stage_error::io_error;

  std::string directory = directory_;
  if (!batch.empty()) {
    const fs::path batch_dir = path_from_utf8(directory_) / path_from_utf8(batch);
    directory = path_to_utf8(batch_dir);
    std::error_code dir_ec;
    fs::create_directories(batch_dir, dir_ec);
    if (dir_ec || !fs::is_directory(batch_dir, dir_ec) || dir_ec) {
      return stage_error::io_error;
    }
  }

  std::string stage_id = random_stage_id();
  while (entries_.find(stage_id) != entries_.end()) {
    stage_id = random_stage_id();
  }
  const std::string final_path = unique_final_path_locked(relative, directory);
  const fs::path final_fs_path = path_from_utf8(final_path);

  staged_clipboard_entry entry;
  entry.id = stage_id;
  entry.name = clean;
  entry.final_path = final_path;
  entry.size = is_dir ? 0 : size;
  entry.received = 0;
  entry.updated_ms = now;
  entry.rel = relative;
  entry.is_directory = is_dir;

  std::error_code ec;
  if (is_dir) {
    // Create the whole chain now: an empty directory has no bytes and nothing
    // later would otherwise create it.
    fs::create_directories(final_fs_path, ec);
    if (ec || !fs::is_directory(final_fs_path, ec) || ec) {
      return stage_error::io_error;
    }
    entry.temp_path = final_path;
  } else {
    const fs::path parent = final_fs_path.parent_path();
    if (!parent.empty()) {
      fs::create_directories(parent, ec);
      if (ec || !fs::is_directory(parent, ec) || ec) {
        return stage_error::io_error;
      }
    }
    const fs::path temp_path =
        path_from_utf8(directory) / path_from_utf8(".stage-" + stage_id);
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) return stage_error::io_error;
    out.close();
    if (!out) return stage_error::io_error;
    entry.temp_path = path_to_utf8(temp_path);
  }

  entries_.emplace(stage_id, std::move(entry));

  id = stage_id;
  path = final_path;
  return stage_error::none;
}

stage_error clipboard_stage::append(const std::string& id, uint64_t offset,
                                    const char* data, std::size_t length,
                                    uint64_t& received) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t now = now_ms();
  sweep_locked(now);
  const auto it = entries_.find(id);
  if (it == entries_.end()) return stage_error::not_found;
  staged_clipboard_entry& entry = it->second;
  if (offset != entry.received) return stage_error::conflict;
  if (length > entry.size - entry.received) return stage_error::too_large;
  if (length > 0) {
    std::ofstream out(path_from_utf8(entry.temp_path),
                      std::ios::binary | std::ios::app);
    if (!out) return stage_error::io_error;
    out.write(data, static_cast<std::streamsize>(length));
    out.flush();
    if (!out) return stage_error::io_error;
  }
  entry.received += length;
  entry.updated_ms = now;
  received = entry.received;
  return stage_error::none;
}

stage_error clipboard_stage::apply(const std::vector<std::string>& ids,
                                   std::size_t& count) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t now = now_ms();
  sweep_locked(now);
  if (directory_.empty()) return stage_error::unavailable;
  if (ids.empty()) return stage_error::bad_request;

  // Validate the whole batch before touching the filesystem, so one stale id
  // cannot leave a partially applied selection on the clipboard.
  for (const std::string& id : ids) {
    const auto it = entries_.find(id);
    if (it == entries_.end()) return stage_error::not_found;
    if (it->second.received != it->second.size) {
      return stage_error::conflict;
    }
  }

  std::vector<std::string> finals;
  finals.reserve(ids.size());
  for (const std::string& id : ids) {
    staged_clipboard_entry& entry = entries_.at(id);
    if (entry.temp_path != entry.final_path) {
      std::error_code ec;
      fs::rename(path_from_utf8(entry.temp_path),
                 path_from_utf8(entry.final_path), ec);
      if (ec) return stage_error::io_error;
      // A retry after a failed applier must not rename again.
      entry.temp_path = entry.final_path;
    }
    finals.push_back(entry.final_path);
  }

  // The clipboard must reference the selection's top-level items, not every
  // descendant: pasting a folder and then each file inside it would duplicate
  // the tree. Files/directories that are already covered by an announced
  // directory are still staged on disk but left out of the clipboard list.
  std::set<std::string> directories;
  for (const std::string& id : ids) {
    const staged_clipboard_entry& entry = entries_.at(id);
    if (entry.is_directory) directories.insert(entry.rel);
  }
  std::vector<std::string> roots;
  roots.reserve(finals.size());
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const staged_clipboard_entry& entry = entries_.at(ids[i]);
    if (is_selection_root(entry.rel, directories)) {
      roots.push_back(finals[i]);
    }
  }
  if (roots.empty()) roots = finals;

  if (!applier_ || !applier_(roots)) return stage_error::apply_failed;
  for (const std::string& id : ids) entries_.erase(id);
  count = roots.size();
  return stage_error::none;
}

void clipboard_stage::abort(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = entries_.find(id);
  if (it == entries_.end()) return;
  std::error_code ec;
  if (it->second.is_directory) {
    // Only succeeds for an empty directory; a partially populated one is left
    // to the batch's normal lifetime.
    fs::remove(path_from_utf8(it->second.final_path), ec);
  } else {
    fs::remove(path_from_utf8(it->second.temp_path), ec);
  }
  entries_.erase(it);
}

void clipboard_stage::sweep() {
  std::lock_guard<std::mutex> lock(mutex_);
  sweep_locked(now_ms());
}

std::size_t clipboard_stage::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

}  // namespace molink
