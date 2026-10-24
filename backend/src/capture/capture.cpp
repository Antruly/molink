#include "capture/capture.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "capture/windows_secure_capture.h"
#include "service_control.h"
#endif

#include <algorithm>
#include <thread>
#include <utility>

namespace molink {

void set_display_awake(bool awake) {
#if defined(_WIN32)
  if (awake) {
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED |
                            ES_SYSTEM_REQUIRED);
    // ES_DISPLAY_REQUIRED only prevents a sleep; the monitor may already be
    // off. A monitor-power -1 broadcast turns it back on.
    SendNotifyMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, -1);
  } else {
    SetThreadExecutionState(ES_CONTINUOUS);
  }
#else
  (void)awake;
#endif
}

const char* quality_mode_name(quality_mode mode) {
  switch (mode) {
    case quality_mode::smooth:
      return "smooth";
    case quality_mode::quality:
      return "quality";
    case quality_mode::turbo:
      return "turbo";
    case quality_mode::raw:
      return "raw";
    case quality_mode::balanced:
    default:
      return "balanced";
  }
}

bool quality_mode_from_name(const std::string& name, quality_mode& out) {
  if (name == "smooth") {
    out = quality_mode::smooth;
    return true;
  }
  if (name == "balanced") {
    out = quality_mode::balanced;
    return true;
  }
  if (name == "quality") {
    out = quality_mode::quality;
    return true;
  }
  if (name == "turbo") {
    out = quality_mode::turbo;
    return true;
  }
  // "超清" and "无损" keep original BGRA pixels with zlib (lossless); the
  // client chooses the frame rate (15 vs 30). "游戏" prioritises frame rate
  // over colour bits. "raw" stays available for clients that want no zlib at
  // all, but the UI does not expose it because a 1080p 30 fps raw stream is
  // far beyond a normal LAN.
  if (name == "raw") {
    out = quality_mode::raw;
    return true;
  }
  if (name == "ultra" || name == "lossless") {
    out = quality_mode::quality;
    return true;
  }
  if (name == "game") {
    out = quality_mode::smooth;
    return true;
  }
  return false;
}

const char* tile_format_name(tile_format format) {
  switch (format) {
    case tile_format::rgb24:
      return "rgb24";
    case tile_format::rgb565:
      return "rgb565";
    case tile_format::gray8:
      return "gray8";
    case tile_format::bgra:
    default:
      return "bgra";
  }
}

quality_profile quality_profile_for(quality_mode mode) {
  quality_profile profile;
  switch (mode) {
    case quality_mode::smooth:
      // 16-bit colour cuts the bytes that must be deflated, inflated and
      // painted; a coarser change threshold drops fine noise as well. The
      // client compensates with a higher capture/pull rate.
      profile.format = tile_format::rgb565;
      profile.effort = compression_effort::fast;
      profile.minimum_change_threshold = 16;
      break;
    case quality_mode::quality:
      // Keep every colour bit, spend more CPU on zlib, and never ignore a
      // visual change that the default threshold would have skipped.
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::best;
      profile.maximum_change_threshold = 1;
      break;
    case quality_mode::turbo:
      // Monochrome and coarse: the absolute minimum for a link that cannot
      // move colour frames at all. The client pairs it with a low frame rate.
      profile.format = tile_format::gray8;
      profile.effort = compression_effort::fast;
      profile.minimum_change_threshold = 24;
      break;
    case quality_mode::raw:
      // Original BGRA pixels, deliberately not deflated. Useful on a fast
      // local link and for the "lossless" presets where the client trades
      // bandwidth for the least possible encode latency.
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::fast;
      profile.compress = false;
      profile.maximum_change_threshold = 1;
      break;
    case quality_mode::balanced:
    default:
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::balanced;
      break;
  }
  return profile;
}

int capture_base::quality_change_threshold(int configured) const {
  const quality_profile profile = quality_profile_for(quality_.load());
  int value = configured;
  if (profile.minimum_change_threshold > 0) {
    value = std::max(value, profile.minimum_change_threshold);
  }
  if (profile.maximum_change_threshold > 0 && value > 0) {
    value = std::min(value, profile.maximum_change_threshold);
  }
  return value;
}

uint16_t choose_tile_size() {
  const unsigned cores = std::thread::hardware_concurrency();
  if (cores >= 8) return 128;
  if (cores >= 4) return 256;
  return 512;
}

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
    // When the session service is installed it launches a SYSTEM helper in the
    // active console session; that helper can also see the Winlogon desktop.
    if (query_session_service().installed && running_in_console_session()) {
      std::unique_ptr<capture_source> secure =
          make_windows_secure_capture(options);
      if (secure) {
        std::string secure_error;
        if (secure->start(secure_error)) {
          note = "capture: secure-desktop";
          return secure;
        }
      }
    }
    std::unique_ptr<capture_source> windows = make_windows_capture(options);
    if (windows) {
      std::string err;
      if (windows->start(err)) {
        note = "capture: " + windows->backend();
        return windows;
      }
      note = "windows capture unavailable: " + err;
    }
    // Windows 7 predates DXGI Desktop Duplication; BitBlt still captures the
    // composited desktop there.
    std::unique_ptr<capture_source> gdi = make_windows_gdi_capture(options);
    if (gdi) {
      std::string err;
      if (gdi->start(err)) {
        note = "capture: " + gdi->backend();
        return gdi;
      }
      note += note.empty() ? "gdi capture unavailable: " + err
                           : "; gdi capture unavailable: " + err;
    }
    if (!note.empty()) note += "; using synthetic frames";
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
