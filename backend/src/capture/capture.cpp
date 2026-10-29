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
    case quality_mode::ultra:
      return "ultra";
    case quality_mode::lossless:
      return "lossless";
    case quality_mode::automatic:
      return "auto";
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
  if (name == "ultra") {
    out = quality_mode::ultra;
    return true;
  }
  if (name == "lossless") {
    out = quality_mode::lossless;
    return true;
  }
  if (name == "auto") {
    out = quality_mode::automatic;
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
      profile.lossless = false;
      profile.av1_qp = 32;
      profile.av1_max_area_percent = 60;
      profile.stream_bits_per_pixel_x1000 = 25;
      break;
    case quality_mode::quality:
      // Keep every colour bit, spend more CPU on zlib, and never ignore a
      // visual change that the default threshold would have skipped.
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::best;
      profile.lossless = true;
      // Near-lossless but still encodable in real time: QP 12 on a bounded
      // area is much faster than QP 4 over the whole screen.
      profile.av1_qp = 12;
      profile.av1_max_area_percent = 20;
      profile.stream_bits_per_pixel_x1000 = 70;
      profile.maximum_change_threshold = 1;
      break;
    case quality_mode::turbo:
      // Monochrome and coarse: the absolute minimum for a link that cannot
      // move colour frames at all. The client pairs it with a low frame rate.
      profile.format = tile_format::gray8;
      profile.effort = compression_effort::fast;
      profile.minimum_change_threshold = 24;
      profile.lossless = false;
      profile.av1_qp = 36;
      profile.av1_grayscale = true;
      profile.stream_bits_per_pixel_x1000 = 15;
      profile.av1_max_area_percent = 60;
      break;
    case quality_mode::ultra:
      // Above "quality": a sharper starting QP for the stream, still within
      // what a GPU encoder can run in real time.
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::best;
      profile.lossless = true;
      profile.av1_qp = 8;
      profile.av1_max_area_percent = 15;
      // The top of the ladder: constant quality, no bitrate ceiling.
      profile.stream_bits_per_pixel_x1000 = 0;
      profile.maximum_change_threshold = 1;
      break;
    case quality_mode::lossless:
      // The top of the ladder: near-lossless, lowest practical QP.
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::best;
      profile.lossless = true;
      profile.av1_qp = 3;
      profile.av1_max_area_percent = 12;
      profile.stream_bits_per_pixel_x1000 = 0;
      profile.maximum_change_threshold = 1;
      break;
    case quality_mode::automatic:
      // Resolved at runtime by effective_quality(); a direct lookup only
      // happens for the raw preset name, so answer with the middle level.
      return quality_profile_for(quality_mode::balanced);
    case quality_mode::raw:
      // Original BGRA pixels, deliberately not deflated. Useful on a fast
      // local link and for the "lossless" presets where the client trades
      // bandwidth for the least possible encode latency.
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::fast;
      profile.compress = false;
      profile.lossless = true;
      profile.av1_qp = 1;
      profile.av1_max_area_percent = 20;
      profile.stream_bits_per_pixel_x1000 = 0;
      profile.maximum_change_threshold = 1;
      // Undeflated BGRA is the most expensive stream there is; giving AV1 up
      // here would not buy a usable frame rate, only a much larger one.
      profile.av1_fallback_to_tiles = false;
      break;
    case quality_mode::balanced:
    default:
      profile.format = tile_format::bgra;
      profile.effort = compression_effort::balanced;
      profile.av1_qp = 24;
      profile.av1_max_area_percent = 35;
      profile.stream_bits_per_pixel_x1000 = 45;
      break;
  }
  return profile;
}

quality_mode capture_base::auto_ladder_mode(int level) {
  static const quality_mode kLadder[kAutoLevels] = {
      quality_mode::smooth, quality_mode::balanced, quality_mode::quality,
      quality_mode::ultra, quality_mode::lossless};
  if (level < 0) level = 0;
  if (level >= kAutoLevels) level = kAutoLevels - 1;
  return kLadder[level];
}

void capture_base::note_viewer_rate(double fps, int target_fps, bool demand,
                                    uint64_t now_ms) {
  const bool auto_mode = quality_.load() == quality_mode::automatic;
  std::lock_guard<std::mutex> lock(rate_mutex_);
  const int level_before = rate_policy_.auto_level;
  // Track a *demand-aware* stall: the viewer paints far below the target while
  // the host is producing frames. A static screen sends nothing and is not a
  // stall, so it never counts here.
  if (demand && fps > 0.0 && target_fps > 0 &&
      fps < static_cast<double>(target_fps) *
                rate_quality_policy::kBehindFraction) {
    if (starved_streak_ < 1000) ++starved_streak_;
  } else {
    starved_streak_ = 0;
  }
  rate_starved_.store(starved_streak_ >= 2);
  if (!rate_policy_.note(fps, target_fps, demand, auto_mode, kAutoLevels,
                         now_ms)) {
    return;
  }
  rate_qp_offset_.store(rate_policy_.qp_offset);
  auto_level_.store(rate_policy_.auto_level);
  if (auto_mode && rate_policy_.auto_level != level_before) {
    // The whole preset moved (tile format, effort, thresholds), so the viewer
    // needs a full picture in the new look, not just a new stream keyframe.
    request_keyframe();
  } else {
    // A changed stream QP re-initialises the encoder, which forces a keyframe;
    // that is why the policy moves in steps with a dwell instead of tracking
    // every sample.
    stream_keyframe_pending_.store(true);
  }
}

int capture_base::quality_change_threshold(int configured) const {
  const quality_profile profile = quality_profile_for(effective_quality());
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

void capture_base::update_cell_clock(const captured_frame& frame) {
  const int cols = (static_cast<int>(frame.width) + 127) / 128;
  const int rows = (static_cast<int>(frame.height) + 127) / 128;
  if (cols <= 0 || rows <= 0) return;
  if (cols != clock_cols_ || rows != clock_rows_) {
    clock_cols_ = static_cast<uint16_t>(cols);
    clock_rows_ = static_cast<uint16_t>(rows);
    cell_clock_.assign(static_cast<std::size_t>(cols) * rows, 0);
    cell_seq_.assign(static_cast<std::size_t>(cols) * rows, 0);
  }
  auto mark = [this, cols, rows, &frame](const std::vector<uint8_t>& payload) {
    if (payload.empty()) return;
    protocol::parsed_frame parsed;
    std::string error;
    if (!protocol::parse_frame_payload(payload.data(), payload.size(), parsed,
                                       error)) {
      return;
    }
    for (const protocol::tile_view& tile : parsed.tiles) {
      if (tile.w == 0 || tile.h == 0) continue;
      const int col0 = tile.x / 128;
      const int col1 = (tile.x + tile.w - 1) / 128;
      const int row0 = tile.y / 128;
      const int row1 = (tile.y + tile.h - 1) / 128;
      for (int row = row0; row <= row1 && row < rows; ++row) {
        for (int col = col0; col <= col1 && col < cols; ++col) {
          cell_clock_[static_cast<std::size_t>(row) * cols + col] =
              frame.ts_us;
          cell_seq_[static_cast<std::size_t>(row) * cols + col] = frame.seq;
        }
      }
    }
  };
  // The base frame has the AV1 rectangles removed, so parse the patch too.
  // Otherwise a lost AV1 patch would never look stale to the clock and the
  // rectangle would stay frozen until an unrelated full keyframe.
  mark(frame.payload);
  mark(frame.av1_payload);
}

bool capture_base::tile_clock(std::vector<uint8_t>& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (clock_cols_ == 0 || clock_rows_ == 0 || cell_clock_.empty()) return false;
  out.resize(8 + cell_clock_.size() * 8);
  auto put16 = [&out](std::size_t at, uint16_t value) {
    out[at] = static_cast<uint8_t>(value & 0xff);
    out[at + 1] = static_cast<uint8_t>(value >> 8);
  };
  put16(0, clock_cols_);
  put16(2, clock_rows_);
  put16(4, 128);
  put16(6, 0);
  for (std::size_t i = 0; i < cell_clock_.size(); ++i) {
    const uint64_t value = cell_clock_[i];
    for (int byte = 0; byte < 8; ++byte) {
      out[8 + i * 8 + byte] =
          static_cast<uint8_t>(value >> (8 * byte));
    }
  }
  return true;
}

av1_stream_encoder* capture_base::stream_for_frame(
    const std::vector<protocol::tile_blob>& tiles, int screen_w, int screen_h,
    bool& keyframe) {
  keyframe = false;
  int64_t changed = 0;
  for (const auto& tile : tiles) {
    changed += static_cast<int64_t>(tile.w) * tile.h;
  }
  const double area = static_cast<double>(screen_w) * screen_h;
  const double moving = area > 0 ? static_cast<double>(changed) / area : 0.0;
  // Field diagnosis: MOLINK_STREAM_FORCE=1 pins the stream path on so a host
  // can be checked without making the screen move.
  // Host setting: how much of the screen must move before the stream takes
  // over. Read here, on the capture thread, so the setting needs no locking.
  const double wanted_enter =
      static_cast<double>(std::clamp(stream_enter_percent_.load(), 1, 100)) /
      100.0;
  if (wanted_enter != applied_enter_fraction_) {
    applied_enter_fraction_ = wanted_enter;
    stream_policy_.set_enter_fraction(wanted_enter);
  }
  const bool forced = std::getenv("MOLINK_STREAM_FORCE") != nullptr;
  // The secure desktop (lock screen, UAC) is captured through the SYSTEM
  // helper at a few frames per second; its tiles never arrive in time to
  // paint, so it always uses the stream, whatever the movement rule or the
  // viewer's codec choice says.
  const bool secure_desktop = is_secure_desktop();
  bool mode_changed = false;
  if (forced || secure_desktop) {
    stream_policy_.armed = true;
    mode_changed = !stream_policy_.streaming;
    stream_policy_.streaming = true;
  } else {
    mode_changed = stream_policy_.note(moving, monotonic_ms());
    if (stream_always_.load() || rate_starved_.load() ||
        monotonic_ms() < stream_nudge_until_.load()) {
      // The tile path is stalling while content is changing: the link cannot
      // carry the tiles, and the stream is the cheaper encoding. Switch even
      // though the movement rule alone would not have.
      stream_policy_.exiting = false;
      if (!stream_policy_.streaming) {
        stream_policy_.streaming = true;
        stream_policy_.entering = false;
        mode_changed = true;
      }
    }
    if (mode_changed && stream_policy_.streaming) {
      // The viewer's decoder has no reference frame for this stream yet.
      stream_keyframe_pending_.store(true);
    } else if (mode_changed) {
      // Leaving the stream: the viewer's tile base is as old as the stream it
      // was painted over, so the first tile frame back has to be a full one.
      // Without this the picture stays half stale until the periodic refresh.
      request_keyframe();
    }
  }
  // Mirror the mode for other threads: a session that joins now uses it to
  // wait for the stream keyframe instead of replaying a tile keyframe.
  stream_live_.store(stream_policy_.streaming);
  // MOLINK_STREAM_LOG=1: one line per mode change, with the movement that
  // caused it. Without this a session that stays on tiles looks identical to
  // one the policy never armed.
  if (mode_changed && std::getenv("MOLINK_STREAM_LOG") != nullptr) {
    std::fprintf(stderr,
                 "host: stream %s (moving=%.1f%%, viewers av1=%d h264=%d "
                 "vp9=%d vp8=%d)\n",
                 stream_policy_.streaming ? "on" : "off", moving * 100.0,
                 av1_stream_allowed_.load() ? 1 : 0,
                 h264_stream_allowed_.load() ? 1 : 0,
                 vp9_stream_allowed_.load() ? 1 : 0,
                 vp8_stream_allowed_.load() ? 1 : 0);
  }
  // Walk the ladder the viewers can decode, best first, and take the first
  // codec this host can actually encode: AV1 stream, then H.264 stream. A
  // viewer set that can decode neither keeps the capture on tiles.
  // Stream bitrate budget from the quality preset, shared by every codec:
  // bit/s = pixels x (bpp/1000) x fps. The top presets return 0, which means
  // constant quality with no ceiling.
  const quality_profile budget_profile =
      quality_profile_for(effective_quality());
  int budget_kbps = 0;
  if (budget_profile.stream_bits_per_pixel_x1000 > 0) {
    const double pixels = static_cast<double>(screen_w) * screen_h;
    const int fps = (std::max)(1, target_fps_.load());
    budget_kbps = static_cast<int>(pixels *
                                  budget_profile.stream_bits_per_pixel_x1000 *
                                  fps / 1000000.0);
    budget_kbps = std::clamp(budget_kbps, 400, 60000);
  }
  std::vector<video_codec> ladder =
      video_ladder(av1_stream_allowed_.load(), h264_stream_allowed_.load(),
                   /*av1_patches_ok=*/false, vp9_stream_allowed_.load(),
                   vp8_stream_allowed_.load());
  if (ladder.empty() && (forced || secure_desktop)) {
    // The secure desktop must stream. If the supervisor has not enabled a
    // codec for this viewer set yet (it evaluates once a second, and the lock
    // can land between ticks), fall back to the two the browsers decode
    // rather than dropping to tiles.
    ladder = video_ladder(/*av1_stream_ok=*/false, /*h264_stream_ok=*/true,
                          /*av1_patches_ok=*/false, /*vp9_stream_ok=*/true,
                          /*vp8_stream_ok=*/false);
  }
  if (ladder.empty()) {
    if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        std::fprintf(stderr, "host: no stream codec every viewer can decode\n");
      }
    }
    return nullptr;
  }
  const bool budget_changed = applied_budget_kbps_ != budget_kbps;
  if (stream_encoder_ && (video_codec_ != ladder.front() || budget_changed)) {
    const bool pending_matches =
        switch_pending_ && next_codec_ == ladder.front() &&
        next_budget_kbps_ == budget_kbps;
    if (pending_matches) {
      // The new encoder exists and its first encode is a keyframe: take over
      // now and let the old bitstream go.
      stream_encoder_ = std::move(stream_encoder_next_);
      video_codec_ = next_codec_;
      applied_budget_kbps_ = next_budget_kbps_;
      switch_pending_ = false;
      stream_keyframe_pending_.store(true);
    } else {
      // Make before break: building the new encoder can take a second on the
      // CPU VP9/VP8 path. Keep the old one producing frames meanwhile, so the
      // viewer's picture never stops updating; the swap happens next frame.
      av1_encoding_settings settings;
      {
        std::lock_guard<std::mutex> lock(av1_settings_mutex_);
        settings = av1_settings_;
      }
      av1_encoder_config config;
      config.device = settings.device;
      config.speed = settings.speed;
      config.threads = settings.threads;
      config.max_bitrate_kbps = budget_kbps;
      for (const video_codec codec : ladder) {
        std::string error;
        if (auto encoder = make_stream_encoder(config, codec, &error)) {
          stream_encoder_next_ = std::move(encoder);
          next_codec_ = codec;
          next_budget_kbps_ = budget_kbps;
          break;
        }
      }
      if (stream_encoder_next_ != nullptr) {
        switch_pending_ = true;
      } else {
        // The new rung cannot be built: keep the old picture instead of
        // dropping to tiles, and let the next evaluation try again.
        stream_keyframe_pending_.store(false);
      }
    }
  }
  if (!stream_policy_.streaming && !stream_policy_.armed) return nullptr;
  // A host without any stream encoder (no GPU, no SVT) would otherwise retry
  // and log on every frame. Look again a few seconds later: the device can
  // show up late (driver ready, GPU freed), but not at frame rate.
  constexpr uint64_t kEncoderRetryMs = 5000;
  if (!stream_encoder_ && encoder_retry_at_ms_ != 0 &&
      monotonic_ms() < encoder_retry_at_ms_) {
    return nullptr;
  }
  if (!stream_encoder_) {
    // Created when the content approaches the threshold, so the switch itself
    // does not pay the encoder start-up.
    av1_encoding_settings settings;
    {
      std::lock_guard<std::mutex> lock(av1_settings_mutex_);
      settings = av1_settings_;
    }
    av1_encoder_config config;
    // The configured device, which defaults to "auto": that prefers the GPU
    // stream encoder and keeps SVT-AV1 behind it. A 1080p screen at the CPU's
    // few hundred milliseconds per frame cannot sustain the moving-picture
    // path the stream exists for, so the host settings still let an operator
    // pin "cpu-svt" when the GPU is reserved for something else.
    config.device = settings.device;
    config.speed = settings.speed;
    config.threads = settings.threads;
    config.max_bitrate_kbps = budget_kbps;
    video_codec chosen = ladder.front();
    for (const video_codec codec : ladder) {
      std::string error;
      if (auto encoder = make_stream_encoder(config, codec, &error)) {
        stream_encoder_ = std::move(encoder);
        chosen = codec;
        break;
      }
      std::fprintf(stderr, "molink: %s\n", error.c_str());
    }
    // Remember a failed attempt so the next frames skip the retry (and the
    // log line). A success clears it.
    encoder_retry_at_ms_ = stream_encoder_ ? 0 : monotonic_ms() + kEncoderRetryMs;
    video_codec_ = chosen;
    applied_budget_kbps_ = stream_encoder_ ? budget_kbps : -1;
    stream_keyframe_pending_.store(true);
  }
  if (std::getenv("MOLINK_STREAM_LOG") != nullptr) {
    static thread_local uint64_t last_mode_log = 0;
    const uint64_t now = monotonic_ms();
    if (now - last_mode_log >= 1000) {
      last_mode_log = now;
      std::fprintf(stderr,
                   "host: capture mode streaming=%d secure=%d ladder=%zu "
                   "encoder=%d backend=%s\n",
                   stream_policy_.streaming ? 1 : 0, secure_desktop ? 1 : 0,
                   ladder.size(), stream_encoder_ ? 1 : 0,
                   backend().c_str());
    }
  }
  if (!stream_policy_.streaming || !stream_encoder_) return nullptr;
  keyframe = stream_keyframe_pending_.load();
  stream_keyframe_pending_.store(false);
  return stream_encoder_.get();
}

bool capture_base::tile_seq(std::vector<uint8_t>& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (clock_cols_ == 0 || clock_rows_ == 0 || cell_seq_.empty()) return false;
  out.resize(8 + cell_seq_.size() * 4);
  auto put16 = [&out](std::size_t at, uint16_t value) {
    out[at] = static_cast<uint8_t>(value & 0xff);
    out[at + 1] = static_cast<uint8_t>(value >> 8);
  };
  put16(0, clock_cols_);
  put16(2, clock_rows_);
  put16(4, 128);
  put16(6, 0);
  for (std::size_t i = 0; i < cell_seq_.size(); ++i) {
    const uint32_t value = cell_seq_[i];
    for (int byte = 0; byte < 4; ++byte) {
      out[8 + i * 4 + byte] = static_cast<uint8_t>(value >> (8 * byte));
    }
  }
  return true;
}

void capture_base::note_build_time(uint64_t us) {
  // A preset change asks the capture thread to forget the previous preset's
  // measurements; the API thread only sets the flag.
  if (av1_budget_reset_.exchange(false)) {
    av1_budget_.reset();
    av1_qp_offset_.store(0);
  }
  // An exponential average keeps one slow keyframe from pinning the value.
  const uint64_t previous = build_time_us_.load();
  const uint64_t average = previous == 0 ? us : (previous * 7 + us) / 8;
  build_time_us_.store(average);

  // A host that cannot encode one frame inside the budget the viewer asked
  // for settles at a handful of frames per second: every frame is sharp, but
  // the picture visibly stutters. Raise the AV1 QP a step at a time until the
  // encoder fits the budget again (see quality_av1_qp()).
  const uint64_t budget_us = frame_budget_us(target_fps_.load());
  const int offset =
      adapt_av1_qp_offset(av1_qp_offset_.load(), average, budget_us);
  av1_qp_offset_.store(offset);

  // Quality cannot be traded away forever: once the QP is at its bound the
  // preset first halves the large rectangles (a quarter of the encode work,
  // and the step that keeps a video moving without the tile path's bytes), and
  // only when that is not enough does it hand the picture to the tiles.
  if (!av1_allowed_.load() ||
      !quality_profile_for(effective_quality()).av1_fallback_to_tiles) {
    return;
  }
  // A viewer that reports dropped frames, or a painted rate far below the
  // request, cannot consume full detail even when the encoder is fast. Halve
  // the large rectangles on both paths while it lasts: the tile path has no
  // other lever on bytes per frame.
  const bool pressure = viewer_pressure_.load();
  tile_downscale_.store(pressure);
  if (pressure && !av1_downscale_.load()) av1_downscale_.store(true);

  const bool over_budget = average > budget_us;
  if (over_budget && offset >= kMaxAv1QpOffset && !av1_downscale_.load()) {
    av1_downscale_.store(true);
    return;  // judge the halved frames on their own
  }
  if (!over_budget && !pressure && av1_downscale_.load() &&
      average * 5 < budget_us * 3) {
    av1_downscale_.store(false);  // comfortable again: restore sharpness
  }
  av1_budget_.note_frame(
      over_budget,
      offset >= kMaxAv1QpOffset && av1_downscale_.load(), monotonic_ms());
}

void capture_base::publish(std::shared_ptr<const captured_frame> frame) {
  stat_frames_.fetch_add(1);
  stat_bytes_.fetch_add(frame->payload.size());
  if (frame->keyframe) stat_keyframes_.fetch_add(1);

  std::lock_guard<std::mutex> lock(mutex_);
  update_cell_clock(*frame);
  latest_ = frame;
  history_bytes_ += frame->payload.size() + frame->av1_payload.size();
  history_.push_back(std::move(frame));
  const uint64_t newest_ts = history_.back()->ts_us;
  while (history_.size() > kHistoryMaxFrames ||
         history_bytes_ > kHistoryMaxBytes ||
         (newest_ts > history_.front()->ts_us &&
          newest_ts - history_.front()->ts_us > kHistoryMaxAgeUs)) {
    const auto& oldest = history_.front();
    history_bytes_ -=
        oldest->payload.size() + oldest->av1_payload.size();
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

uint32_t next_capture_seq() {
  static std::atomic<uint32_t> seq{0};
  return ++seq;
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
    // active console session; that helper is the only source that can see the
    // Winlogon desktop. On a normal Default desktop its GDI capture is slower
    // and misses DWM-composited regions (stale wallpaper blocks), so prefer
    // DXGI there and keep the helper only for the lock/login screen.
    if (query_session_service().installed && running_in_console_session()) {
      std::unique_ptr<capture_source> secure =
          make_windows_secure_capture(options);
      if (secure) {
        std::string secure_error;
        if (secure->start(secure_error)) {
          if (secure->is_secure_desktop()) {
            note = "capture: secure-desktop";
            return secure;
          }
          secure->stop();
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
