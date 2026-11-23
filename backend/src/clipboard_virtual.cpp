#include "clipboard_virtual.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>

#include "clipboard_stage.h"

namespace molink {
namespace {

namespace fs = std::filesystem;

uint32_t read_u32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t read_u64(const uint8_t* data) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | data[i];
  }
  return value;
}

/// Names are shown inside one flat directory; reuse the staging sanitizer so a
/// remote name cannot escape it, then make duplicates unique.
std::string unique_name(const std::string& raw,
                        const std::set<std::string>& taken) {
  const std::string name = clipboard_stage::sanitize_name(raw);
  if (name.empty()) return std::string();
  if (taken.find(name) == taken.end()) return name;
  const fs::path path(name);
  const std::string stem = path.stem().string();
  const std::string extension = path.extension().string();
  for (int i = 1; i < 10000; ++i) {
    const std::string candidate =
        stem + " (" + std::to_string(i) + ")" + extension;
    if (taken.find(candidate) == taken.end()) return candidate;
  }
  return std::string();
}

}  // namespace

virtual_offer_error plan_virtual_offer(
    const std::vector<virtual_offer_request_file>& requested,
    std::vector<virtual_mount_file>& planned, std::size_t& omitted) {
  planned.clear();
  omitted = 0;
  std::set<std::string> names;
  std::set<uint32_t> ids;
  uint64_t total = 0;
  std::size_t index = 0;
  for (; index < requested.size(); ++index) {
    const virtual_offer_request_file& file = requested[index];
    if (planned.size() >= kVirtualMaxFiles) break;
    if (file.id == 0 || ids.find(file.id) != ids.end()) continue;
    if (file.size > kVirtualMaxFileBytes) break;
    if (file.size > kVirtualMaxTotalBytes - total) break;
    const std::string name = unique_name(file.name, names);
    if (name.empty()) continue;
    virtual_mount_file entry;
    entry.id = file.id;
    entry.name = name;
    entry.size = file.size;
    planned.push_back(std::move(entry));
    names.insert(name);
    ids.insert(file.id);
    total += file.size;
  }
  omitted = requested.size() - planned.size();
  if (planned.empty()) return virtual_offer_error::bad_file;
  return virtual_offer_error::none;
}

virtual_clipboard::virtual_clipboard(std::string cache_root,
                                     virtual_mount_factory factory,
                                     apply_fn apply, clear_fn clear,
                                     post_fn post, uint64_t pull_timeout_ms,
                                     uint64_t read_timeout_ms)
    : cache_root_(std::move(cache_root)),
      factory_(std::move(factory)),
      apply_(std::move(apply)),
      clear_cb_(std::move(clear)),
      post_(std::move(post)),
      pull_timeout_ms_(pull_timeout_ms == 0 ? kVirtualPullTimeoutMs
                                            : pull_timeout_ms),
      read_timeout_ms_(read_timeout_ms == 0 ? kVirtualReadTimeoutMs
                                            : read_timeout_ms) {}

virtual_clipboard::~virtual_clipboard() { clear("shutdown"); }

bool virtual_clipboard::available() const {
  return factory_ != nullptr && apply_ != nullptr && !cache_root_.empty();
}

bool virtual_clipboard::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mount_ != nullptr;
}

std::string virtual_clipboard::active_mount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mount_id_;
}

void virtual_clipboard::sweep_stale_locked() {
  std::vector<std::string> keep;
  for (const std::string& point : stale_dirs_) {
    if (platform_path_is_mountpoint(point)) {
      keep.push_back(point);
      continue;
    }
    std::error_code ec;
    fs::remove_all(point, ec);
  }
  stale_dirs_.swap(keep);
  if (stale_dirs_.empty()) {
    std::error_code ec;
    fs::remove(cache_root_, ec);
  }
}

void virtual_clipboard::clear_locked() {
  std::shared_ptr<virtual_mount> mount;
  std::string point;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mount = std::move(mount_);
    point = std::move(mountpoint_);
    mountpoint_.clear();
    mount_id_.clear();
    session_.clear();
    files_.clear();
    cache_.clear();
    cache_order_.clear();
    cache_bytes_ = 0;
    send_ = nullptr;
    bridge_ = false;
    bridge_id_ = 0;
    for (auto& item : pending_) item.second->failed = true;
    pending_.clear();
  }
  cv_.notify_all();
  // Give waiting FUSE reads a moment to return -EIO before the mount goes
  // away. Otherwise the kernel tears the connection down first and the file
  // manager sees ENOTCONN instead.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (readers_waiting_ > 0) {
      drain_cv_.wait_for(lock, std::chrono::milliseconds(500),
                         [this] { return readers_waiting_ == 0; });
    }
  }
  bool detached = true;
  if (mount) detached = mount->stop();
  if (!point.empty()) {
    // Never walk a directory that is still a mountpoint: with the FUSE daemon
    // gone, stat()/readdir() on it waits in the kernel forever.
    if (detached || !platform_path_is_mountpoint(point)) {
      std::error_code ec;
      fs::remove_all(point, ec);
      // Drop the now-empty `~/.cache/molink/clip/` root too; the acceptance
      // check is "no residue", not "an empty directory".
      fs::remove(cache_root_, ec);
    } else {
      stale_dirs_.push_back(point);
    }
  }
  sweep_stale_locked();
  // Only touch the clipboard when this generation owned it. A first offer with
  // no previous mount must not blank whatever the user had copied.
  if (mount && clear_cb_) clear_cb_();
}

void virtual_clipboard::clear(const std::string&) {
  std::lock_guard<std::mutex> lifecycle(lifecycle_);
  clear_locked();
}

bool virtual_clipboard::clear_if(const std::string& mount) {
  std::lock_guard<std::mutex> lifecycle(lifecycle_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mount_id_ != mount || mount_ == nullptr) return false;
  }
  clear_locked();
  return true;
}

virtual_offer_error virtual_clipboard::offer(
    const std::string& session,
    const std::vector<virtual_offer_request_file>& files,
    std::size_t& omitted, std::string& mount, std::vector<std::string>& paths,
    std::string& error) {
  omitted = 0;
  mount.clear();
  paths.clear();
  error.clear();
  if (!available()) {
    error = "this build has no virtual clipboard (libfuse3 missing)";
    return virtual_offer_error::unavailable;
  }

  std::vector<virtual_mount_file> planned;
  virtual_offer_error planned_error =
      plan_virtual_offer(files, planned, omitted);
  if (planned_error != virtual_offer_error::none) {
    error = "the announcement had no usable file";
    return planned_error;
  }

  std::lock_guard<std::mutex> lifecycle(lifecycle_);
  sweep_stale_locked();
  clear_locked();

  const uint64_t generation = ++next_generation_;
  const std::string id = std::to_string(generation);
  const fs::path point = fs::path(cache_root_) / id;
  std::error_code ec;
  fs::create_directories(point, ec);
  if (ec || !fs::is_directory(point, ec)) {
    error = "cannot create the mount directory";
    return virtual_offer_error::mount_failed;
  }

  std::string mount_error;
  std::shared_ptr<virtual_mount> created =
      factory_(point.string(), planned, this, mount_error);
  if (!created || !created->mounted()) {
    bool detached = true;
    if (created) detached = created->stop();
    if (detached || !platform_path_is_mountpoint(point.string())) {
      fs::remove_all(point, ec);
    } else {
      stale_dirs_.push_back(point.string());
    }
    error = mount_error.empty() ? "the FUSE mount failed" : mount_error;
    return virtual_offer_error::mount_failed;
  }

  std::map<uint32_t, virtual_mount_file> by_id;
  for (const virtual_mount_file& file : planned) {
    by_id[file.id] = file;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mount_ = created;
    mountpoint_ = point.string();
    mount_id_ = id;
    session_ = session;
    files_ = std::move(by_id);
    send_ = nullptr;
    bridge_ = false;
    bridge_id_ = 0;
  }

  std::vector<std::string> local_paths;
  local_paths.reserve(planned.size());
  for (const virtual_mount_file& file : planned) {
    local_paths.push_back((point / file.name).string());
  }
  // The state is visible before the clipboard is claimed: a paste that starts
  // the instant set_files() queues a wakeup must find the entries.
  if (!apply_ || !apply_(local_paths)) {
    clear_locked();
    error = "the local clipboard refused the virtual paths";
    return virtual_offer_error::clipboard_unavailable;
  }
  mount = id;
  paths = std::move(local_paths);
  return virtual_offer_error::none;
}

bool virtual_clipboard::bridge_attach(const std::string& mount,
                                      send_text_fn send_text,
                                      uint64_t* bridge_id) {
  std::vector<std::string> replay;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mount_id_ != mount || !mount_) return false;
    send_ = send_text;
    bridge_ = true;
    bridge_id_ = next_bridge_id_++;
    if (bridge_id != nullptr) *bridge_id = bridge_id_;
    // A paste may have started before the page finished opening the socket.
    // Replay those requests instead of making them wait out the timeout.
    for (const auto& item : pending_) {
      const std::shared_ptr<pending_read>& pending = item.second;
      if (pending->done || pending->failed) continue;
      char message[160];
      std::snprintf(message, sizeof(message),
                    "{\"type\":\"pull\",\"seq\":%u,\"id\":%u,\"offset\":%llu,"
                    "\"length\":%llu}",
                    item.first, pending->file_id,
                    static_cast<unsigned long long>(pending->offset),
                    static_cast<unsigned long long>(pending->length));
      replay.emplace_back(message);
    }
  }
  if (!replay.empty() && send_text) {
    for (const std::string& message : replay) send_text(message);
  }
  return true;
}

void virtual_clipboard::bridge_detach(const std::string& mount,
                                      uint64_t bridge_id) {
  std::lock_guard<std::mutex> lifecycle(lifecycle_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mount_id_ != mount) return;
    if (bridge_id != 0 && bridge_id != bridge_id_) return;
  }
  // No bridge means no bytes: every waiting paste must fail now rather than
  // hang for the full 15 s, and the mount is useless without its transport.
  clear_locked();
}

void virtual_clipboard::bridge_binary(const uint8_t* data,
                                      std::size_t length) {
  if (data == nullptr || length < 16) return;
  const uint32_t seq = read_u32(data);
  const uint32_t status = read_u32(data + 4);
  const uint64_t offset = read_u64(data + 8);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pending_.find(seq);
    if (it == pending_.end()) return;
    const std::shared_ptr<pending_read>& pending = it->second;
    if (pending->done || pending->failed) return;
    const std::size_t bytes = length - 16;
    if (status != 0 || offset != pending->offset ||
        bytes != pending->length) {
      pending->failed = true;
    } else {
      try {
        pending->data.assign(data + 16, data + length);
      } catch (...) {
        pending->data.clear();
        pending->failed = true;
      }
    }
    pending->done = true;
  }
  cv_.notify_all();
}

int virtual_clipboard::pull_block(
    uint32_t file_id, uint64_t offset, uint64_t length, char* buffer,
    std::size_t capacity, std::chrono::steady_clock::time_point total_deadline) {
  uint32_t seq = 0;
  uint64_t take = 0;
  bool deliver = false;
  send_text_fn send;
  post_fn post;
  std::shared_ptr<pending_read> pending;
  std::string message;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mount_) return -5;  // EIO
    // A read can only be served by a live session bridge. Without one the pull
    // would wait out the whole deadline and then report EIO, which shows up as
    // a file manager frozen for a minute mid-paste; fail while the paste has
    // not started instead.
    if (!bridge_) return -5;
    const auto it = files_.find(file_id);
    if (it == files_.end()) return -5;
    if (offset >= it->second.size) return 0;
    take = std::min<uint64_t>(length, it->second.size - offset);
    take = std::min<uint64_t>(take, capacity);
    take = std::min<uint64_t>(take, kVirtualPullBytes);
    if (take == 0) return 0;
    const auto cached = cache_.find({file_id, offset});
    if (cached != cache_.end() && cached->second.length == take) {
      std::memcpy(buffer, cached->second.data.data(), take);
      return static_cast<int>(take);
    }
    // Overlapping readahead can ask for the same range before the first reply
    // arrives. Share that request instead of sending the host a duplicate.
    for (const auto& item : pending_) {
      const std::shared_ptr<pending_read>& other = item.second;
      if (other->file_id == file_id && other->offset == offset &&
          other->length == take && !other->done && !other->failed) {
        seq = item.first;
        pending = other;
        break;
      }
    }
    if (!pending) {
      seq = next_seq_++;
      pending = std::make_shared<pending_read>();
      pending->file_id = file_id;
      pending->offset = offset;
      pending->length = take;
      pending_.emplace(seq, pending);
      deliver = bridge_ && send_ != nullptr;
      send = send_;
      post = post_;
      char text[160];
      std::snprintf(text, sizeof(text),
                    "{\"type\":\"pull\",\"seq\":%u,\"id\":%u,\"offset\":%llu,"
                    "\"length\":%llu}",
                    seq, file_id, static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(take));
      message = text;
    }
    ++readers_waiting_;
  }

  bool delivered = true;
  if (deliver) {
    try {
      if (post) {
        std::weak_ptr<virtual_clipboard> weak = weak_from_this();
        post([weak, seq, message] {
          auto self = weak.lock();
          if (!self) return;
          send_text_fn target;
          {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (!self->bridge_ || !self->send_) return;
            const auto it = self->pending_.find(seq);
            if (it == self->pending_.end() || it->second->done ||
                it->second->failed) {
              return;
            }
            target = self->send_;
          }
          try {
            if (target) target(message);
          } catch (...) {
            // The page's bridge is the only transport for these bytes. A
            // throwing send must fail the pending read, not escape the loop
            // (or the FUSE reader thread) and terminate the process.
            std::lock_guard<std::mutex> lock(self->mutex_);
            const auto it = self->pending_.find(seq);
            if (it != self->pending_.end()) it->second->failed = true;
            self->cv_.notify_all();
          }
        });
      } else if (send) {
        send(message);
      }
    } catch (...) {
      delivered = false;
    }
  }
  if (!delivered) {
    // The request never left this process; undo the bookkeeping the waiter
    // below would otherwise do and fail the read immediately.
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pending_.find(seq);
    if (it != pending_.end()) pending_.erase(it);
    if (readers_waiting_ > 0) --readers_waiting_;
    drain_cv_.notify_all();
    return -5;
  }

  const auto chunk_deadline = std::min(
      total_deadline,
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds(pull_timeout_ms_));
  std::unique_lock<std::mutex> lock(mutex_);
  const bool ready = cv_.wait_until(lock, chunk_deadline, [&pending] {
    return pending->done || pending->failed;
  });
  const auto it = pending_.find(seq);
  if (it != pending_.end()) pending_.erase(it);
  if (readers_waiting_ > 0) --readers_waiting_;
  drain_cv_.notify_all();
  if (!ready || pending->failed || pending->data.size() != take) return -5;
  if (cache_.find({file_id, offset}) == cache_.end()) {
    cached_range entry;
    entry.length = take;
    entry.data = pending->data;
    cache_bytes_ += take;
    cache_.emplace(std::make_pair(file_id, offset), std::move(entry));
    cache_order_.emplace_back(file_id, offset);
    while (cache_bytes_ > kCacheMaxBytes && !cache_order_.empty()) {
      const std::pair<uint32_t, uint64_t> oldest = cache_order_.front();
      cache_order_.pop_front();
      const auto evicted = cache_.find(oldest);
      if (evicted == cache_.end()) continue;
      cache_bytes_ -= evicted->second.data.size();
      cache_.erase(evicted);
    }
  }
  std::memcpy(buffer, pending->data.data(), take);
  return static_cast<int>(take);
}

int virtual_clipboard::read_file_range(uint32_t file_id, uint64_t offset,
                                       uint64_t length, char* buffer,
                                       std::size_t capacity) {
  if (buffer == nullptr) return -5;
  if (capacity < length) length = capacity;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(read_timeout_ms_);
  // FUSE calls this on its worker thread. Any exception (a broken bridge, a
  // bad allocation, a page-side send throwing) must become -EIO so the file
  // manager shows a failed copy instead of the whole core disappearing.
  try {
    uint64_t done = 0;
    while (done < length) {
      const uint64_t want =
          std::min<uint64_t>(length - done, kVirtualPullBytes);
      const int got = pull_block(file_id, offset + done, want, buffer + done,
                                 capacity - done, deadline);
      if (got < 0) return -5;
      done += static_cast<uint64_t>(got);
      if (static_cast<uint64_t>(got) < want) break;  // end of file
    }
    return static_cast<int>(done);
  } catch (...) {
    return -5;
  }
}

}  // namespace molink
