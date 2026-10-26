#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "capture/frame_encode.h"

namespace molink {

/// User-facing quality preset. The client resolves "auto" locally and sends
/// one of these three concrete modes; keeping auto out of the wire protocol
/// means a new heuristic does not need a core update.
enum class quality_mode : uint8_t {
  smooth,    ///< lossy 16-bit colour, fast zlib, higher frame rate
  balanced,  ///< lossless BGRA with the adaptive zlib policy
  quality,   ///< lossless BGRA, best zlib effort
  turbo,     ///< 8-bit grayscale, coarse changes, low frame rate
  raw,       ///< lossless BGRA without zlib (the client picks the frame rate)
};

struct quality_profile {
  tile_format format = tile_format::bgra;
  compression_effort effort = compression_effort::balanced;
  /// False for the "lossless" preset: send original BGRA tiles untouched.
  bool compress = true;
  /// True when this preset must stay bit-exact (BGRA, optionally deflated).
  /// The AV1 rectangle path is only allowed when this is false: SVT-AV1 is
  /// 4:2:0 and even QP 0 is not lossless.
  bool lossless = true;
  /// Raise the configured change threshold to at least this value (0 = leave).
  int minimum_change_threshold = 0;
  /// Lower a non-zero change threshold to at most this value (0 = leave).
  int maximum_change_threshold = 0;
};

const char* quality_mode_name(quality_mode mode);
bool quality_mode_from_name(const std::string& name, quality_mode& out);
const char* tile_format_name(tile_format format);
quality_profile quality_profile_for(quality_mode mode);

/// One captured screen update, already encoded as a FRAME payload.
///
/// The payload is built on the capture thread so the event loop only has to
/// prepend a packet header. `keyframe` means every tile is present; otherwise
/// only tiles that changed since the previous frame are included.
struct captured_frame {
  uint32_t seq = 0;
  uint64_t ts_us = 0;
  bool keyframe = false;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t tile_w = 0;
  uint16_t tile_h = 0;
  uint8_t codec = 0;  ///< protocol::codec of the payload
  std::vector<uint8_t> payload;
};

/// The pointer cursor as the display reports it. Cursor pixels are not part of
/// a screen grab on X11, so clients draw this separately.
struct cursor_snapshot {
  bool valid = false;
  uint32_t serial = 0;  ///< changes whenever the cursor shape changes
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t hotspot_x = 0;
  uint16_t hotspot_y = 0;
  int x = 0;  ///< pointer position in screen pixels
  int y = 0;
  std::vector<uint8_t> rgba;  ///< width * height * 4, non-premultiplied
};

/// One selectable monitor as reported by the capture backend. `width/height`
/// are capture pixels; `input_width/input_height` are the units the input
/// injector uses (points on macOS, desktop pixels on X11/Windows).
struct display_info {
  int index = 0;
  std::string name;
  int x = 0;
  int y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  int input_width = 0;
  int input_height = 0;
  bool primary = false;
};

struct capture_stats {  uint64_t frames = 0;
  uint64_t keyframes = 0;
  uint64_t full_grabs = 0;
  uint64_t rect_grabs = 0;
  uint64_t bytes = 0;
  uint16_t last_dirty_w = 0;
  uint16_t last_dirty_h = 0;
};

struct capture_options {
  int fps = 15;
  uint16_t tile_size = 256;
  /// Compress tiles with zlib when the frame is compressible.
  bool compress = true;
  // Synthetic source dimensions (also the fallback when no display is
  // reachable). The X11 source uses the real screen size instead.
  uint16_t synthetic_width = 640;
  uint16_t synthetic_height = 360;
  /// Per-channel delta that still counts as unchanged when diffing tiles.
  /// 0 = exact memcmp. A small value ignores anti-aliasing and encoder noise.
  int change_threshold = 4;
  /// Monitor index to capture initially (0 = first/primary).
  int display_index = 0;
};

/// Picks a tile edge from the detected CPU width: smaller tiles spread updates
/// more evenly and lower latency on strong machines; larger tiles reduce
/// per-tile overhead on weak ones.
uint16_t choose_tile_size();

class capture_source {
 public:
  virtual ~capture_source() = default;

  virtual bool start(std::string& err) = 0;
  virtual void stop() = 0;
  virtual bool running() const = 0;

  /// Short identifier: "x11-shm", "x11", "synthetic".
  virtual std::string backend() const = 0;

  /// Most recent encoded frame, or nullptr before the first one.
  virtual std::shared_ptr<const captured_frame> latest() const = 0;

  /// Frame with exactly this capture sequence, or nullptr once it has been
  /// evicted from the history. Clients resuming after a stall replay from here
  /// instead of forcing a new keyframe.
  virtual std::shared_ptr<const captured_frame> find(uint32_t seq) const = 0;

  /// Ask the source to mark every tile in the next frame. Used when a client
  /// has no base image yet, or missed a frame.
  virtual void request_keyframe() = 0;

  /// Capture only while at least one client is watching. A static desktop with
  /// no viewers should not cost a core.
  virtual void set_active(bool active) = 0;

  /// Change the target capture rate (1..60). Used by the adaptive quality
  /// controller when a client reports high latency or low bandwidth. Sources
  /// that cannot pace themselves may ignore this.
  virtual void set_target_fps(int fps) { (void)fps; }

  /// Select the encoding preset. Global to the host, like the capture rate:
  /// the next frame for every session uses it. Sources that build their own
  /// payloads (the synthetic pattern) may ignore this.
  virtual void set_quality(quality_mode mode) { (void)mode; }
  virtual quality_mode quality() const { return quality_mode::balanced; }

  /// Monitors this source can capture. Empty means the source is not ready or
  /// does not support selection (clients fall back to the whole screen).
  virtual std::vector<display_info> displays() const { return {}; }
  virtual int current_display() const { return 0; }
  virtual display_info current_display_info() const { return {}; }

  /// Requests a monitor switch. The capture thread applies it and publishes a
  /// keyframe. Returns false for an unknown index.
  virtual bool select_display(int index) {
    return index == 0;
  }

  /// Change the current display mode. Windows applies it with
  /// ChangeDisplaySettingsEx; this only works when the driver already exposes
  /// the requested mode (virtual display drivers usually do). Returns false
  /// when the platform or driver cannot apply it.
  virtual bool set_display_mode(uint16_t width, uint16_t height) {
    (void)width;
    (void)height;
    return false;
  }

  /// True when set_display_mode() can do something on this backend. The
  /// session advertises this so clients do not offer a resolution menu that
  /// is guaranteed to fail (the secure-desktop, GDI and X11 sources cannot
  /// change the mode).
  virtual bool supports_display_mode() const { return false; }

  /// True when frames come from a real display rather than the test pattern.
  virtual bool is_real() const = 0;

  virtual capture_stats stats() const = 0;

  /// Latest cursor shape and position, or false when the backend cannot report
  /// one (synthetic frames, missing XFixes).
  virtual bool cursor(cursor_snapshot& out) const = 0;

  /// Copy the most recent full screen image as BGRA rows. Used to serve a
  /// tile-level refresh without forcing a keyframe; backends that cannot
  /// provide it return false and the caller falls back to a keyframe.
  virtual bool copy_pixels(std::vector<uint8_t>& out, int& width, int& height) {
    (void)out;
    (void)width;
    (void)height;
    return false;
  }
};

/// Shared bookkeeping for concrete sources: latest-frame slot, keyframe
/// request flag, running flag.
/// Keep the local display awake while a remote viewer is connected, and wake a
/// monitor that already powered off. No-op on platforms without an equivalent.
void set_display_awake(bool awake);

class capture_base : public capture_source {
 public:
  explicit capture_base(int fps = 15) : target_fps_(clamp_fps(fps)) {}

  std::shared_ptr<const captured_frame> latest() const override;
  std::shared_ptr<const captured_frame> find(uint32_t seq) const override;
  void request_keyframe() override { keyframe_requested_.store(true); }
  void set_active(bool active) override {
    active_.store(active);
    set_display_awake(active);
  }
  void set_target_fps(int fps) override { target_fps_.store(clamp_fps(fps)); }
  int target_fps() const { return target_fps_.load(); }
  void set_quality(quality_mode mode) override { quality_.store(mode); }
  quality_mode quality() const override { return quality_.load(); }

  /// Encoding knobs for the active preset. Backends read these on the capture
  /// thread, which is why the preset is an atomic.
  tile_format quality_format() const {
    return quality_profile_for(quality_.load()).format;
  }
  compression_effort quality_effort() const {
    return quality_profile_for(quality_.load()).effort;
  }
  bool quality_compress() const {
    return quality_profile_for(quality_.load()).compress;
  }
  /// Apply the preset's bounds to the configured change threshold.
  int quality_change_threshold(int configured) const;

  bool running() const override { return running_.load(); }
  virtual capture_stats stats() const override;

  bool cursor(cursor_snapshot& out) const override;

  /// Hard bounds for the replay history, whichever comes first. Partial frames
  /// are usually small; the byte cap exists so a burst of full-screen changes
  /// cannot grow memory without limit.
  static constexpr std::size_t kHistoryMaxBytes = 128u * 1024u * 1024u;
  static constexpr std::size_t kHistoryMaxFrames = 120;

 protected:
  static int clamp_fps(int fps) { return fps < 1 ? 1 : (fps > 60 ? 60 : fps); }

  void publish(std::shared_ptr<const captured_frame> frame);
  void note_grab(bool full, uint16_t dirty_w, uint16_t dirty_h);
  void set_cursor(cursor_snapshot snapshot);

  mutable std::mutex mutex_;
  std::shared_ptr<const captured_frame> latest_;
  std::deque<std::shared_ptr<const captured_frame>> history_;
  std::size_t history_bytes_ = 0;

  mutable std::mutex cursor_mutex_;
  cursor_snapshot cursor_;
  std::atomic<bool> keyframe_requested_{false};
  std::atomic<bool> active_{false};
  std::atomic<bool> running_{false};
  std::atomic<int> target_fps_{15};
  std::atomic<quality_mode> quality_{quality_mode::balanced};
  std::atomic<uint64_t> stat_frames_{0};
  std::atomic<uint64_t> stat_keyframes_{0};
  std::atomic<uint64_t> stat_full_grabs_{0};
  std::atomic<uint64_t> stat_rect_grabs_{0};
  std::atomic<uint64_t> stat_bytes_{0};
  std::atomic<uint16_t> stat_dirty_w_{0};
  std::atomic<uint16_t> stat_dirty_h_{0};
};

/// Returns nullptr when the build has no X11 support.
std::unique_ptr<capture_source> make_x11_capture(const capture_options& options);

/// Returns nullptr when the build does not target Windows.
std::unique_ptr<capture_source> make_windows_capture(
    const capture_options& options);

/// BitBlt fallback for Windows 7, which predates DXGI Desktop Duplication.
/// Returns nullptr when the build does not target Windows.
std::unique_ptr<capture_source> make_windows_gdi_capture(
    const capture_options& options);

/// Returns nullptr when the build does not target macOS.
std::unique_ptr<capture_source> make_macos_capture(
    const capture_options& options);
std::unique_ptr<capture_source> make_synthetic_capture(const capture_options& options);

/// Starts a real display source when possible, otherwise the synthetic one.
/// `note` always receives a human-readable description for logs and /api/info.
std::shared_ptr<capture_source> start_capture(const capture_options& options,
                                              bool allow_display,
                                              std::string& note);

}  // namespace molink
