#include "clipboard_stage.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>
#include <utility>

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
  fs::create_directories(directory_, ec);
  if (ec) return false;
  return fs::is_directory(directory_, ec) && !ec;
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
  const fs::path dir(directory);
  auto taken = [this](const fs::path& candidate) {
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec) return true;
    const std::string text = candidate.filename().string();
    for (const auto& item : entries_) {
      const fs::path existing(item.second.final_path);
      // Names only collide inside one directory; `a.txt` in another batch's
      // folder is a different file.
      if (existing.parent_path() == candidate.parent_path() &&
          same_name(existing.filename().string(), text)) {
        return true;
      }
    }
    return false;
  };

  fs::path candidate = dir / name;
  if (!taken(candidate)) return candidate.string();
  const fs::path stem = fs::path(name).stem();
  const std::string extension = fs::path(name).extension().string();
  for (int i = 1; i < 10000; ++i) {
    candidate =
        dir / (stem.string() + " (" + std::to_string(i) + ")" + extension);
    if (!taken(candidate)) return candidate.string();
  }
  candidate = dir / (stem.string() + "-" + std::to_string(now_ms()) +
                     extension);
  return candidate.string();
}

void clipboard_stage::sweep_locked(uint64_t now) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (now > it->second.updated_ms &&
        now - it->second.updated_ms >= kEntryTtlMs) {
      std::error_code ec;
      if (it->second.temp_path != it->second.final_path) {
        fs::remove(it->second.temp_path, ec);
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
    const std::string name = entry.path().filename().string();
    std::error_code time_ec;
    const auto written = entry.last_write_time(time_ec);
    if (time_ec) return;
    const bool pending = name.compare(0, 7, ".stage-") == 0;
    const uint64_t ttl = pending ? kEntryTtlMs : kAppliedTtlMs;
    if (written + std::chrono::milliseconds(ttl) < file_now) {
      fs::remove(entry.path(), time_ec);
    }
  };
  fs::directory_iterator it(directory_, ec);
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
                                   const std::string& batch) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t now = now_ms();
  sweep_locked(now);
  if (directory_.empty()) return stage_error::unavailable;
  if (size > kMaxFileBytes) return stage_error::too_large;
  if (!batch.empty() && !valid_batch(batch)) return stage_error::bad_request;
  const std::string clean = sanitize_name(name);
  if (clean.empty()) return stage_error::bad_request;
  if (!ensure_directory_locked()) return stage_error::io_error;

  std::string directory = directory_;
  if (!batch.empty()) {
    directory = (fs::path(directory_) / batch).string();
    std::error_code dir_ec;
    fs::create_directories(directory, dir_ec);
    if (dir_ec || !fs::is_directory(directory, dir_ec) || dir_ec) {
      return stage_error::io_error;
    }
  }

  std::string stage_id = random_stage_id();
  while (entries_.find(stage_id) != entries_.end()) {
    stage_id = random_stage_id();
  }
  const std::string final_path = unique_final_path_locked(clean, directory);
  const fs::path temp_path = fs::path(directory) / (".stage-" + stage_id);

  std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
  if (!out) return stage_error::io_error;
  out.close();
  if (!out) return stage_error::io_error;

  staged_clipboard_entry entry;
  entry.id = stage_id;
  entry.name = clean;
  entry.final_path = final_path;
  entry.temp_path = temp_path.string();
  entry.size = size;
  entry.received = 0;
  entry.updated_ms = now;
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
    std::ofstream out(entry.temp_path,
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
      fs::rename(entry.temp_path, entry.final_path, ec);
      if (ec) return stage_error::io_error;
      // A retry after a failed applier must not rename again.
      entry.temp_path = entry.final_path;
    }
    finals.push_back(entry.final_path);
  }

  if (!applier_ || !applier_(finals)) return stage_error::apply_failed;
  for (const std::string& id : ids) entries_.erase(id);
  count = finals.size();
  return stage_error::none;
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
