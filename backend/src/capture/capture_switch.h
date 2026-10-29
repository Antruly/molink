#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "capture/capture.h"

namespace molink {

/// A capture source that delegates to another one and can swap it at runtime.
///
/// Windows picks its capture backend once at startup: DXGI on the normal
/// desktop, the SYSTEM helper on the lock/UAC desktop, where DXGI cannot see
/// anything. The desktop changes in the middle of a session (a lock, a UAC
/// prompt), and without a swap the picture freezes while input keeps working
/// through the helper. This proxy forwards everything to the current source
/// and replays the settings it was given onto a freshly built one.
class capture_switch final : public capture_source {
 public:
  /// Builds a source for the desktop that is in front now. Returning nullptr
  /// keeps the current source, so a failed switch never blanks the picture.
  using factory =
      std::function<std::shared_ptr<capture_source>(bool secure_desktop)>;

  capture_switch(std::shared_ptr<capture_source> initial, factory make,
                 bool secure_desktop);

  /// Builds a source for @p secure_desktop and swaps it in. The previous
  /// source is stopped after the swap. False when the build failed.
  bool switch_desktop(bool secure_desktop);

  bool secure_desktop() const { return secure_.load(); }

  /// The source currently in front, for diagnostics.
  std::shared_ptr<capture_source> inner() const;

  // -- capture_source -----------------------------------------------------
  bool start(std::string& err) override;
  void stop() override;
  bool running() const override;
  std::string backend() const override;
  tile_format frame_format() const override;
  std::shared_ptr<const captured_frame> latest() const override;
  std::shared_ptr<const captured_frame> find(uint32_t seq) const override;
  void request_keyframe() override;
  void set_active(bool active) override;
  void set_target_fps(int fps) override;
  int target_fps() const override;
  uint64_t build_time_us() const override;
  void set_quality(quality_mode mode) override;
  quality_mode quality() const override;
  void set_av1_allowed(bool allowed) override;
  void set_av1_settings(const av1_encoding_settings& settings) override;
  void set_keyframe_interval_ms(int ms) override;
  bool tile_clock(std::vector<uint8_t>& out) override;
  bool tile_seq(std::vector<uint8_t>& out) override;
  void set_viewer_pressure(bool pressure) override;
  void note_viewer_rate(double fps, int target_fps, bool demand,
                        uint64_t now_ms) override;
  void set_av1_stream_allowed(bool allowed) override;
  void set_stream_enter_percent(int percent) override;
  void set_h264_stream_allowed(bool allowed) override;
  void request_stream(uint64_t ms) override;
  bool stream_on() const override;
  void set_stream_always(bool always) override;
  void set_vp9_stream_allowed(bool allowed) override;
  void set_vp8_stream_allowed(bool allowed) override;
  std::vector<display_info> displays() const override;
  int current_display() const override;
  display_info current_display_info() const override;
  bool select_display(int index) override;
  bool set_display_mode(uint16_t width, uint16_t height) override;
  bool supports_display_mode() const override;
  bool is_real() const override;
  bool is_secure_desktop() const override;
  capture_stats stats() const override;
  bool cursor(cursor_snapshot& out) const override;
  bool copy_pixels(std::vector<uint8_t>& out, int& width, int& height) override;

 private:
  /// Runs @p fn against the current source, if there is one.
  template <typename Fn>
  void with(Fn fn) const {
    const std::shared_ptr<capture_source> source = inner();
    if (source != nullptr) fn(*source);
  }
  /// Re-applies the settings collected while the swap was in progress.
  void replay_settings(capture_source& source) const;

  mutable std::mutex mutex_;
  std::shared_ptr<capture_source> inner_;
  factory make_;
  std::atomic<bool> secure_{false};

  // Settings cached so a freshly built backend starts in the same state as the
  // one it replaces.
  std::atomic<bool> active_{false};
  std::atomic<int> target_fps_{0};
  std::atomic<int> quality_{static_cast<int>(quality_mode::balanced)};
  std::atomic<bool> av1_allowed_{false};
  std::atomic<int> keyframe_interval_ms_{0};
  std::atomic<bool> pressure_{false};
  std::atomic<bool> av1_stream_allowed_{false};
  std::atomic<bool> h264_stream_allowed_{false};
  std::atomic<bool> vp9_stream_allowed_{false};
  std::atomic<bool> vp8_stream_allowed_{false};
  std::atomic<bool> stream_always_{false};
  std::atomic<int> stream_enter_percent_{15};
  /// Deadline of a request_stream() window, so a source swap does not lose it.
  std::atomic<uint64_t> stream_nudge_until_{0};
  av1_encoding_settings av1_settings_;
  mutable std::mutex settings_mutex_;
};

}  // namespace molink
