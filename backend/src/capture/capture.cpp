#include "capture/capture.h"

#include <utility>

namespace molink {

std::shared_ptr<const captured_frame> capture_base::latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

std::shared_ptr<const captured_frame> capture_base::find(uint32_t seq) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // Newest first: a client normally wants the frame right after its last one.
  for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
    if ((*it)->seq == seq) return *it;
    if ((*it)->seq < seq) break;
  }
  return nullptr;
}

void capture_base::publish(std::shared_ptr<const captured_frame> frame) {
  stat_frames_.fetch_add(1);
  stat_bytes_.fetch_add(frame->payload.size());
  if (frame->keyframe) stat_keyframes_.fetch_add(1);

  std::lock_guard<std::mutex> lock(mutex_);
  latest_ = frame;
  history_bytes_ += frame->payload.size();
  history_.push_back(std::move(frame));
  while (history_.size() > kHistoryMaxFrames ||
         history_bytes_ > kHistoryMaxBytes) {
    const auto& oldest = history_.front();
    history_bytes_ -= oldest->payload.size();
    history_.pop_front();
  }
}

void capture_base::note_grab(bool full, uint16_t dirty_w, uint16_t dirty_h) {
  if (full) {
    stat_full_grabs_.fetch_add(1);
  } else {
    stat_rect_grabs_.fetch_add(1);
  }
  stat_dirty_w_.store(dirty_w);
  stat_dirty_h_.store(dirty_h);
}

void capture_base::set_cursor(cursor_snapshot snapshot) {
  std::lock_guard<std::mutex> lock(cursor_mutex_);
  cursor_ = std::move(snapshot);
}

bool capture_base::cursor(cursor_snapshot& out) const {
  std::lock_guard<std::mutex> lock(cursor_mutex_);
  if (!cursor_.valid) return false;
  out = cursor_;
  return true;
}

capture_stats capture_base::stats() const {
  capture_stats s;
  s.frames = stat_frames_.load();
  s.keyframes = stat_keyframes_.load();
  s.full_grabs = stat_full_grabs_.load();
  s.rect_grabs = stat_rect_grabs_.load();
  s.bytes = stat_bytes_.load();
  s.last_dirty_w = stat_dirty_w_.load();
  s.last_dirty_h = stat_dirty_h_.load();
  return s;
}

std::shared_ptr<capture_source> start_capture(const capture_options& options,
                                              bool allow_display,
                                              std::string& note) {
  if (allow_display) {
#if defined(_WIN32)
    std::unique_ptr<capture_source> windows = make_windows_capture(options);
    if (windows) {
      std::string err;
      if (windows->start(err)) {
        note = "capture: " + windows->backend();
        return windows;
      }
      note = "windows capture unavailable: " + err + "; using synthetic frames";
    }
#elif defined(__APPLE__)
    std::unique_ptr<capture_source> macos = make_macos_capture(options);
    if (macos) {
      std::string err;
      if (macos->start(err)) {
        note = "capture: " + macos->backend();
        return macos;
      }
      note = "macos capture unavailable: " + err + "; using synthetic frames";
    }
#elif MOLINK_HAVE_X11
    std::unique_ptr<capture_source> x11 = make_x11_capture(options);
    if (x11) {
      std::string err;
      if (x11->start(err)) {
        note = "capture: " + x11->backend();
        return x11;
      }
      note = "x11 capture unavailable: " + err + "; using synthetic frames";
    }
#else
    note = "built without X11 capture; using synthetic frames";
#endif
  } else {
    note = "capture disabled by configuration; using synthetic frames";
  }

  std::unique_ptr<capture_source> synthetic = make_synthetic_capture(options);
  std::string err;
  if (!synthetic->start(err)) {
    note = "synthetic capture failed: " + err;
    return nullptr;
  }
  if (note.empty()) note = "capture: synthetic";
  return synthetic;
}

}  // namespace molink
