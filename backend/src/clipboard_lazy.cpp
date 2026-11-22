#include "clipboard_lazy.h"

#include <algorithm>
#include <chrono>
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

}  // namespace

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
    for (const std::size_t index : to_request) request(index);
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
    std::shared_ptr<lazy_file_source> source) {
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
