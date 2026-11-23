#include "clipboard_lazy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace molink {
namespace {

using steady_clock = std::chrono::steady_clock;

bool valid_name(const std::string& name) {
  if (name.empty() || name.size() > 255) return false;
  for (const unsigned char c : name) {
    if (c == 0) return false;
    if (c < 0x20 || c == 0x7f) return false;
    // The name becomes one component of the host's staging path. A separator
    // or a drive/stream colon would let a peer choose that path.
    if (c == '/' || c == '\\' || c == ':') return false;
  }
  return name != "." && name != "..";
}

/// Request ids are process-global rather than per offer: a late reply from a
/// replaced offer must never satisfy a new offer's fetch that happened to
/// allocate the same number. Wrapping after 4 billion fetches is harmless in
/// practice; zero stays reserved for "no request".
uint32_t next_range_id() {
  static std::atomic<uint32_t> counter{1};
  for (;;) {
    const uint32_t id = counter.fetch_add(1, std::memory_order_relaxed);
    if (id != 0) return id;
  }
}

}  // namespace

bool lazy_blob_seek(uint64_t size, uint64_t position, int64_t move,
                    lazy_seek_origin origin, uint64_t& new_position) {
  int64_t base = 0;
  switch (origin) {
    case lazy_seek_origin::begin:
      base = 0;
      break;
    case lazy_seek_origin::current:
      base = static_cast<int64_t>(position);
      break;
    case lazy_seek_origin::end:
      base = static_cast<int64_t>(size);
      break;
    default:
      return false;
  }
  // The files this protocol carries are capped far below INT64_MAX; refusing
  // an overflowing request is still cheaper than wrapping the offset.
  if (move > 0 && base > std::numeric_limits<int64_t>::max() - move) {
    return false;
  }
  if (move < 0 && base < std::numeric_limits<int64_t>::min() - move) {
    return false;
  }
  const int64_t target = base + move;
  if (target < 0) return false;
  new_position = static_cast<uint64_t>(target);
  return true;
}

bool read_lazy_blob(lazy_blob_source& source, std::size_t index,
                    uint64_t& position, uint8_t* buffer, uint32_t bytes,
                    uint32_t& read, std::string& error,
                    uint64_t chunk_timeout_ms, uint64_t total_timeout_ms) {
  read = 0;
  error.clear();
  if (buffer == nullptr && bytes != 0) {
    error = "null read buffer";
    return false;
  }
  if (index >= source.size()) {
    error = "clipboard file index out of range";
    return false;
  }
  const uint64_t size = source.files()[index].size;
  const steady_clock::time_point deadline =
      steady_clock::now() + std::chrono::milliseconds(total_timeout_ms);
  while (read < bytes) {
    if (position >= size) break;  // clean end of file
    const uint64_t want =
        std::min<uint64_t>({static_cast<uint64_t>(bytes - read), size - position,
                            kLazyStreamBlockBytes});
    const steady_clock::time_point now = steady_clock::now();
    if (now >= deadline) {
      error = "clipboard file read timed out";
      return false;
    }
    const uint64_t left_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());
    std::vector<uint8_t> chunk;
    std::string range_error;
    bool ranged = false;
    try {
      ranged = source.read_range(index, position, static_cast<uint32_t>(want),
                                 chunk, range_error,
                                 std::min(chunk_timeout_ms, left_ms), left_ms);
    } catch (const std::exception& exception) {
      // IStream::Read is called by Explorer's paste thread. An exception here
      // must become STG_E_READFAULT, never std::terminate.
      range_error = exception.what();
      ranged = false;
    } catch (...) {
      range_error = "clipboard file read threw";
      ranged = false;
    }
    if (!ranged) {
      error = range_error.empty() ? "clipboard file read failed" : range_error;
      return false;
    }
    if (chunk.size() > want) {
      error = "clipboard range reply is larger than requested";
      return false;
    }
    if (!chunk.empty()) {
      std::memcpy(buffer + read, chunk.data(), chunk.size());
      read += static_cast<uint32_t>(chunk.size());
      position += chunk.size();
    }
    // A short (or empty) chunk means the viewer reached end of file. Its
    // descriptor size may be stale if the file shrank after it was copied.
    if (chunk.size() < want) break;
  }
  return true;
}

lazy_offer_error validate_lazy_offer(
    const std::vector<lazy_clipboard_file>& files) {
  if (files.empty()) return lazy_offer_error::bad_file;
  if (files.size() > kMaxLazyFiles) return lazy_offer_error::too_many_files;
  uint64_t total = 0;
  for (const lazy_clipboard_file& file : files) {
    if (!valid_name(file.name)) return lazy_offer_error::bad_file;
    if (file.size > kMaxLazyBytes - total) {
      return lazy_offer_error::too_large;
    }
    total += file.size;
  }
  return lazy_offer_error::none;
}

lazy_file_source::lazy_file_source(std::vector<lazy_clipboard_file> files,
                                   request_fn request)
    : files_(std::move(files)), request_(std::move(request)) {
  states_.resize(files_.size());
}

lazy_file_source::~lazy_file_source() { fail_all("destroyed"); }

bool lazy_file_source::ensure(std::size_t index, std::string& path,
                              uint64_t total_timeout_ms,
                              uint64_t chunk_timeout_ms) {
  std::vector<std::string> paths;
  if (!wait_files({index}, paths, total_timeout_ms, chunk_timeout_ms)) {
    return false;
  }
  path = paths.front();
  return true;
}

bool lazy_file_source::ensure_all(std::vector<std::string>& paths,
                                  uint64_t total_timeout_ms,
                                  uint64_t chunk_timeout_ms) {
  if (files_.empty()) return false;
  std::vector<std::size_t> indices;
  indices.reserve(files_.size());
  for (std::size_t i = 0; i < files_.size(); ++i) indices.push_back(i);
  return wait_files(indices, paths, total_timeout_ms, chunk_timeout_ms);
}

bool lazy_file_source::wait_files(const std::vector<std::size_t>& indices,
                                  std::vector<std::string>& paths,
                                  uint64_t total_timeout_ms,
                                  uint64_t chunk_timeout_ms) {
  request_fn request;
  std::vector<std::size_t> to_request;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    const steady_clock::time_point now = steady_clock::now();
    for (const std::size_t index : indices) {
      if (index >= states_.size()) return false;
      file_state& state = states_[index];
      if (state.complete) continue;
      if (state.failed) return false;
      if (!state.requested) {
        state.requested = true;
        state.started_at = now;
        state.last_progress = now;
        to_request.push_back(index);
      }
    }
    request = request_;
  }

  // Never call back into the session while holding the mutex: the callback
  // posts to the event loop, and the upload progress it causes re-enters
  // note_progress().
  if (!to_request.empty()) {
    if (!request) {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const std::size_t index : to_request) states_[index].failed = true;
      return false;
    }
    for (const std::size_t index : to_request) {
      try {
        request(index);
      } catch (const std::exception& exception) {
        // The request posts to the session's event loop. A throwing post must
        // fail this file, not escape into the clipboard render thread.
        (void)exception;
        std::lock_guard<std::mutex> lock(mutex_);
        states_[index].failed = true;
        cv_.notify_all();
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        states_[index].failed = true;
        cv_.notify_all();
      }
    }
  }

  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    bool all_complete = true;
    steady_clock::time_point deadline = steady_clock::time_point::max();
    const steady_clock::time_point now = steady_clock::now();
    for (const std::size_t index : indices) {
      const file_state& state = states_[index];
      if (state.complete) continue;
      all_complete = false;
      if (state.failed || closed_) return false;
      const steady_clock::time_point total_deadline =
          state.started_at +
          std::chrono::milliseconds(total_timeout_ms);
      const steady_clock::time_point block_deadline =
          state.last_progress +
          std::chrono::milliseconds(chunk_timeout_ms);
      deadline = std::min(deadline, std::min(total_deadline, block_deadline));
    }
    if (all_complete) {
      paths.clear();
      paths.reserve(indices.size());
      for (const std::size_t index : indices) {
        paths.push_back(states_[index].path);
      }
      return true;
    }
    if (now >= deadline) {
      // One file missed its whole-file or per-block budget. CF_HDROP needs
      // every path, and a stream needs the file it asked for, so the batch
      // fails as a whole instead of returning a partial set.
      for (const std::size_t index : indices) {
        file_state& state = states_[index];
        if (state.complete) continue;
        const steady_clock::time_point total_deadline =
            state.started_at + std::chrono::milliseconds(total_timeout_ms);
        const steady_clock::time_point block_deadline =
            state.last_progress + std::chrono::milliseconds(chunk_timeout_ms);
        if (now >= total_deadline || now >= block_deadline) {
          state.failed = true;
        }
      }
      return false;
    }
    cv_.wait_until(lock, deadline);
  }
}

void lazy_file_source::note_progress(std::size_t index, uint64_t received) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_ || index >= states_.size()) return;
  file_state& state = states_[index];
  if (state.complete || state.failed) return;
  if (received > state.received) state.received = received;
  state.last_progress = steady_clock::now();
  cv_.notify_all();
}

void lazy_file_source::note_done(std::size_t index,
                                 const std::string& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_ || index >= states_.size()) return;
  file_state& state = states_[index];
  if (state.complete) return;
  if (path.empty()) {
    state.failed = true;
  } else {
    state.complete = true;
    state.received = files_[index].size;
    state.path = path;
  }
  cv_.notify_all();
}

void lazy_file_source::note_failed(std::size_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_ || index >= states_.size()) return;
  states_[index].failed = true;
  cv_.notify_all();
}

void lazy_file_source::fail_all(const std::string&) {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = true;
  cv_.notify_all();
}

bool lazy_file_source::closed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return closed_;
}

remote_file_source::remote_file_source(std::vector<lazy_clipboard_file> files,
                                       range_request_fn request)
    : files_(std::move(files)), request_(std::move(request)) {}

remote_file_source::~remote_file_source() { fail_all("destroyed"); }

bool remote_file_source::read_range(std::size_t index, uint64_t offset,
                                    uint32_t length, std::vector<uint8_t>& out,
                                    std::string& error,
                                    uint64_t chunk_timeout_ms,
                                    uint64_t total_timeout_ms) {
  out.clear();
  error.clear();
  if (index >= files_.size()) {
    error = "clipboard file index out of range";
    return false;
  }
  const uint64_t size = files_[index].size;
  if (offset >= size) return true;  // reading at EOF is not an error
  const uint64_t available = size - offset;
  const uint32_t want = static_cast<uint32_t>(
      std::min<uint64_t>(static_cast<uint64_t>(length), available));
  if (want == 0) return true;

  auto wait = std::make_shared<pending>();
  wait->offset = offset;
  wait->length = want;
  uint32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      error = "clipboard offer is closed";
      return false;
    }
    id = next_range_id();
    pending_[id] = wait;
  }

  // Never call the transport while holding mutex_: the request posts to the
  // session's event loop, and a very fast peer's reply re-enters note_range()
  // from that loop.
  bool posted = false;
  try {
    posted = request_ && request_(index, id, offset, want);
  } catch (const std::exception& exception) {
    error = exception.what();
    posted = false;
  } catch (...) {
    error = "clipboard range request threw";
    posted = false;
  }
  if (!posted) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(id);
    error = "clipboard range request was not sent";
    return false;
  }

  const steady_clock::time_point started = steady_clock::now();
  const steady_clock::time_point chunk_deadline =
      started + std::chrono::milliseconds(chunk_timeout_ms);
  const steady_clock::time_point total_deadline =
      started + std::chrono::milliseconds(total_timeout_ms);
  const steady_clock::time_point deadline =
      std::min(chunk_deadline, total_deadline);

  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (wait->done) {
      pending_.erase(id);
      if (wait->error != 0) {
        error = "peer could not read the clipboard file";
        return false;
      }
      if (wait->reply_offset != offset) {
        error = "clipboard range reply has the wrong offset";
        return false;
      }
      if (wait->data.size() > want) {
        error = "clipboard range reply is larger than requested";
        return false;
      }
      out = std::move(wait->data);
      return true;
    }
    if (closed_) {
      pending_.erase(id);
      error = "clipboard offer is closed";
      return false;
    }
    const steady_clock::time_point now = steady_clock::now();
    if (now >= deadline) {
      pending_.erase(id);
      error = now >= total_deadline ? "clipboard file read timed out"
                                    : "clipboard range reply timed out";
      return false;
    }
    cv_.wait_until(lock, deadline);
  }
}

void remote_file_source::note_range(uint32_t id, uint32_t error,
                                    uint64_t offset, const uint8_t* data,
                                    std::size_t length) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = pending_.find(id);
  if (it == pending_.end()) return;  // timed out, or a previous offer's reply
  const std::shared_ptr<pending>& wait = it->second;
  if (wait->done) return;
  wait->reply_offset = offset;
  wait->error = error;
  if (error == 0) {
    if (data == nullptr && length != 0) {
      wait->error = 1;
    } else if (length > wait->length) {
      // A buggy peer must not make the host allocate more than it asked for.
      wait->error = 1;
    } else if (length != 0) {
      try {
        wait->data.assign(data, data + length);
      } catch (...) {
        wait->data.clear();
        wait->error = 1;
      }
    }
  }
  wait->done = true;
  cv_.notify_all();
}

bool remote_file_source::has_pending(uint32_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !closed_ && pending_.find(id) != pending_.end();
}

std::size_t remote_file_source::pending_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_.size();
}

void remote_file_source::fail_all(const std::string&) {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = true;
  cv_.notify_all();
}

bool remote_file_source::closed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return closed_;
}

lazy_clipboard::lazy_clipboard(clock_fn clock) : clock_(std::move(clock)) {}

uint64_t lazy_clipboard::now_ms() const {
  if (clock_) return clock_();
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
}

lazy_offer_error lazy_clipboard::offer(
    uint32_t session_id, std::string dest,
    std::vector<lazy_clipboard_file> files,
    std::shared_ptr<lazy_blob_source> source) {
  const lazy_offer_error error = validate_lazy_offer(files);
  if (error != lazy_offer_error::none) return error;
  if (session_id == 0 || !source) return lazy_offer_error::no_session;

  auto offer = std::make_shared<lazy_offer>();
  offer->session_id = session_id;
  offer->dest = std::move(dest);
  offer->files = std::move(files);
  offer->source = std::move(source);
  offer->created_ms = now_ms();

  std::shared_ptr<lazy_offer> replaced;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    replaced = std::move(offer_);
    offer_ = std::move(offer);
  }
  if (replaced && replaced->source) {
    replaced->source->fail_all("replaced by a newer clipboard offer");
  }
  return lazy_offer_error::none;
}

bool lazy_clipboard::clear_session(uint32_t session_id) {
  std::shared_ptr<lazy_offer> cleared;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!offer_ || offer_->session_id != session_id) return false;
    cleared = std::move(offer_);
  }
  if (cleared && cleared->source) {
    cleared->source->fail_all("session closed");
  }
  return true;
}

void lazy_clipboard::clear_all(const std::string& reason) {
  std::shared_ptr<lazy_offer> cleared;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cleared = std::move(offer_);
  }
  if (cleared && cleared->source) cleared->source->fail_all(reason);
}

std::shared_ptr<const lazy_offer> lazy_clipboard::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return offer_;
}

bool lazy_clipboard::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return offer_ != nullptr;
}

bool lazy_clipboard::active_for(uint32_t session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return offer_ && offer_->session_id == session_id;
}

uint32_t lazy_clipboard::session_id() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return offer_ ? offer_->session_id : 0;
}

}  // namespace molink
