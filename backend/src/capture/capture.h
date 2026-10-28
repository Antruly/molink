#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "capture/av1_encoder.h"
#include "capture/frame_budget.h"
#include "capture/stream_policy.h"
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
  /// True when this preset wants bit-exact tiles. AV1 still runs for large
  /// rectangles on these presets, but at a near-lossless QP; small changes
  /// (text, UI) stay on the bit-exact tile path.
  bool lossless = true;
  /// AV1 quantizer for this preset: 1 is near-lossless, 63 the worst. SVT's
  /// minimum accepted QP is 1 in 4.x.
  int av1_qp = 24;
  /// The turbo preset is monochrome; AV1 must encode grayscale pixels.
  bool av1_grayscale = false;
  /// Upper bound on the AV1-encoded area per frame, as a percentage of the
  /// screen. Encoding a full 2520x1680 frame at a near-lossless QP cannot fit
  /// a capture frame budget, so the rest stays on the tile path. Ignored by
  /// full-frame mode, where every changed rectangle must be covered.
  int av1_max_area_percent = 100;
  /// Allow the capture to drop AV1 and fall back to zlib tiles when the
  /// encoder cannot keep up with the viewer's frame rate even at the coarsest
  /// QP (see av1_budget_policy). Tiles are lossless but several times larger on
  /// the wire, so presets that prioritise the smallest stream opt out.
  bool av1_fallback_to_tiles = true;
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
  /// Optional AV1 picture (a complete FRAME payload with codec::av1). Covers
  /// every changed rectangle when full-frame AV1 is on, otherwise only the
  /// large ones that were removed from `payload`. Empty when AV1 is off or
  /// nothing qualified.
  std::vector<uint8_t> av1_payload;
  /// True when `av1_payload` is the whole picture and `payload` carries no
  /// tiles. The session then sends only the AV1 packet; sending both would
  /// waste the link on a duplicate image.
  bool av1_only = false;
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

/// Host-level AV1 knobs from the settings page. `qp` and `max_area_percent`
/// are 0 when the active quality preset should decide.
struct av1_encoding_settings {
  std::string device = "auto";
  std::string speed = "auto";
  int qp = 0;
  int max_area_percent = 0;
  int threads = 0;
  /// AV1-only frames: every changed rectangle goes through the AV1 encoder and
  /// the tile payload is empty. Tiles are then only used when AV1 cannot run
  /// (no encoder, a viewer without a decoder, or an encode failure).
  bool full_frame = true;
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

  /// Tile format the active preset produces, for recovery paths that build
  /// tiles by hand (a refresh must not paint colour over a monochrome preset).
  virtual tile_format frame_format() const { return tile_format::bgra; }

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

  /// The rate last requested by a client. 0 when the source does not pace.
  virtual int target_fps() const { return 0; }

  /// Smoothed time spent building and encoding one frame, in microseconds.
  /// Clients use it to tell a slow host encoder from a slow link; 0 when the
  /// source does not report it.
  virtual uint64_t build_time_us() const { return 0; }

  /// Select the encoding preset. Global to the host, like the capture rate:
  /// the next frame for every session uses it. Sources that build their own
  /// payloads (the synthetic pattern) may ignore this.
  virtual void set_quality(quality_mode mode) { (void)mode; }
  virtual quality_mode quality() const { return quality_mode::balanced; }

  /// Allow AV1 patches for the current session set. The session layer turns
  /// this on only when every watching session can decode AV1; the preset must
  /// also be a lossy one, since AV1 cannot preserve pixels bit-exactly.
  virtual void set_av1_allowed(bool allowed) { (void)allowed; }

  /// Host preference for the AV1 encoder (device, speed, QP and area caps).
  /// Applied on the capture thread with the next frame.
  virtual void set_av1_settings(const av1_encoding_settings& settings) {
    (void)settings;
  }

  /// Forced full-keyframe cadence in milliseconds. 0 disables it; backends may
  /// still run their built-in safety refresh. 1000 means one keyframe per
  /// second (the most real-time, at a large bandwidth cost).
  virtual void set_keyframe_interval_ms(int ms) { (void)ms; }

  /// Per-128px-cell capture timestamps of the newest pixels, as a binary
  /// tile-clock payload. False when the source has no grid yet.
  virtual bool tile_clock(std::vector<uint8_t>& out) {
    (void)out;
    return false;
  }

  /// Per-128px-cell capture sequence of the newest pixels, as a binary
  /// tile-sequence payload. Clients that advertised `seq_clock` use it to name
  /// the cells they missed exactly, without a timestamp window.
  virtual bool tile_seq(std::vector<uint8_t>& out) {
    (void)out;
    return false;
  }

  /// A viewer reported that it cannot keep up (dropped frames or a painted
  /// rate far below the request). The capture then sends less detail instead
  /// of waiting for the viewer's own frame-rate controller to give up.
  virtual void set_viewer_pressure(bool pressure) { (void)pressure; }

  /// Whether the AV1 low-delay stream may be used. Only set while every live
  /// session can decode it.
  virtual void set_av1_stream_allowed(bool allowed) { (void)allowed; }

  /// Whether a hardware H.264 stream may be used instead. This is the fallback
  /// for viewers that cannot decode AV1 (the browser decodes H.264 through
  /// WebCodecs); AV1 still wins when every viewer can decode it.
  virtual void set_h264_stream_allowed(bool allowed) { (void)allowed; }

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

  /// True only for the Windows secure-desktop helper. Normal desktop sessions
  /// prefer DXGI even when the service is installed; the helper is reserved
  /// for Winlogon/the lock screen, where DXGI cannot capture.
  virtual bool is_secure_desktop() const { return false; }

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
  void request_keyframe() override {
    keyframe_requested_.store(true);
    // A viewer that joins (or lost the stream) cannot decode a delta: the
    // stream has to restart from a keyframe.
    stream_keyframe_pending_.store(true);
  }
  void set_active(bool active) override {
    active_.store(active);
    set_display_awake(active);
  }
  void set_target_fps(int fps) override { target_fps_.store(clamp_fps(fps)); }
  int target_fps() const override { return target_fps_.load(); }
  void set_quality(quality_mode mode) override {
    // Repeated pushes of the same preset (the viewer re-sends it whenever its
    // frame rate changes) must not restart the encoder-quality adaptation,
    // otherwise it never gathers a streak. Only a real change resets it.
    if (quality_.exchange(mode) == mode) return;
    av1_qp_offset_.store(0);
    av1_downscale_.store(false);
    av1_budget_reset_.store(true);
    // Frames in the replay history were encoded with the previous preset
    // (colour vs monochrome, a different QP). Replaying one into a viewer that
    // has already switched would paint the old look back.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      history_.clear();
      history_bytes_ = 0;
    }
  }
  quality_mode quality() const override { return quality_.load(); }

  /// Encoding knobs for the active preset. Backends read these on the capture
  /// thread, which is why the preset is an atomic.
  tile_format quality_format() const {
    return quality_profile_for(quality_.load()).format;
  }
  /// Recovery paths that build tiles by hand (a tile refresh after a dropped
  /// frame) must use the preset's format, or a monochrome session gets colour
  /// patches painted over it.
  tile_format frame_format() const override { return quality_format(); }
  compression_effort quality_effort() const {
    return quality_profile_for(quality_.load()).effort;
  }
  bool quality_compress() const {
    return quality_profile_for(quality_.load()).compress;
  }
  /// Apply the preset's bounds to the configured change threshold.
  int quality_change_threshold(int configured) const;

  void set_av1_allowed(bool allowed) override {
    av1_allowed_.store(allowed);
  }
  uint64_t build_time_us() const override { return build_time_us_.load(); }
  void set_av1_settings(const av1_encoding_settings& settings) override {
    std::lock_guard<std::mutex> lock(av1_settings_mutex_);
    av1_settings_ = settings;
  }
  void set_keyframe_interval_ms(int ms) override {
    periodic_keyframe_ms_.store(ms < 0 ? 0 : ms);
  }
  /// The forced cadence; backends fall back to 30 s when this is 0.
  int periodic_keyframe_ms() const { return periodic_keyframe_ms_.load(); }
  /// Encoder to use for this frame, or nullptr when AV1 is off. Only the
  /// capture thread calls this, so the lazily created encoder needs no lock.
  /// A device/speed/thread change recreates it on the next frame.
  av1_encoder* av1_for_frame() {
    if (!av1_allowed_.load()) return nullptr;
    bool resumed = false;
    if (!av1_budget_.allow_av1(monotonic_ms(), resumed)) return nullptr;
    if (resumed) {
      // A retry starts at the coarsest QP the preset allows, so the decision
      // costs a handful of frames rather than a slow minute.
      av1_qp_offset_.store(kMaxAv1QpOffset);
    }
    av1_encoding_settings settings;
    {
      std::lock_guard<std::mutex> lock(av1_settings_mutex_);
      settings = av1_settings_;
    }
    const bool same_config =
        encoder_configured_ && settings.device == encoder_device_ &&
        settings.speed == encoder_speed_ &&
        settings.threads == encoder_threads_;
    if (av1_encoder_ && same_config) return av1_encoder_.get();
    if (!av1_encoder_ && same_config) return nullptr;
    av1_encoder_.reset();
    av1_encoder_config config;
    config.device = settings.device;
    config.speed = settings.speed;
    config.threads = settings.threads;
    std::string error;
    av1_encoder_ = make_av1_encoder(config, &error);
    encoder_configured_ = true;
    encoder_device_ = settings.device;
    encoder_speed_ = settings.speed;
    encoder_threads_ = settings.threads;
    if (!av1_encoder_ && !error.empty()) {
      std::fprintf(stderr, "molink: %s\n", error.c_str());
    }
    return av1_encoder_.get();
  }
  int quality_av1_qp() const {
    av1_encoding_settings settings;
    {
      std::lock_guard<std::mutex> lock(av1_settings_mutex_);
      settings = av1_settings_;
    }
    // An explicit QP is the host owner's choice, so it is taken literally.
    if (settings.qp >= 1 && settings.qp <= 63) return settings.qp;
    const int base = quality_profile_for(quality_.load()).av1_qp;
    // The near-lossless presets keep their QP: that quality is the point of
    // choosing them, and the frame rate controller already reacts to a slow
    // encoder by lowering the rate.
    if (base <= 4) return base;
    // CPU-only hosts cannot encode a sharp 1080p intra frame at the rate the
    // viewer asks for. Raising the QP in small steps trades some sharpness
    // for a picture that actually moves; see note_build_time().
    return (std::min)(base + av1_qp_offset_.load(), kMaxAv1Qp);
  }
  /// QP for the AV1 stream. It deliberately ignores the frame-time
  /// adaptation: every QP change re-initialises the encoder and forces a
  /// keyframe, which would restart the stream again and again.
  int quality_av1_qp_stream() const {
    av1_encoding_settings settings;
    {
      std::lock_guard<std::mutex> lock(av1_settings_mutex_);
      settings = av1_settings_;
    }
    if (settings.qp >= 1 && settings.qp <= 63) return settings.qp;
    return quality_profile_for(quality_.load()).av1_qp;
  }
  bool quality_av1_grayscale() const {
    return quality_profile_for(quality_.load()).av1_grayscale;
  }
  /// True when screen-sized AV1 rectangles should be encoded at half
  /// resolution. The policy turns this on only after raising the QP was not
  /// enough, and off again when the encoder is comfortably inside the budget.
  bool quality_av1_downscale() const { return av1_downscale_.load(); }
  /// Hysteresis deadline for the full-screen AV1 picture (see encode_options).
  uint64_t* av1_full_screen_until() { return &av1_full_screen_until_us_; }
  /// True when large tile strips should be box-averaged in place: the viewer
  /// is behind and this is the tile path's only lever on bytes per frame.
  bool quality_tile_downscale() const { return tile_downscale_.load(); }

  void set_viewer_pressure(bool pressure) override {
    viewer_pressure_.store(pressure);
  }
  void set_av1_stream_allowed(bool allowed) override {
    av1_stream_allowed_.store(allowed);
  }
  void set_h264_stream_allowed(bool allowed) override {
    h264_stream_allowed_.store(allowed);
  }
  /// True while the AV1 stream is the active path. The backend should then
  /// publish a frame on every tick, even when nothing changed: a video stream
  /// needs a steady cadence, and a P frame of an unchanged screen costs a few
  /// kilobytes.
  bool stream_active() const { return stream_policy_.streaming; }
  /// Feeds the moving-area policy for this frame. Returns the stream encoder
  /// to use, or null while the frame stays on tiles; `keyframe` asks for a
  /// stream keyframe (the first frame after a switch).
  av1_stream_encoder* stream_for_frame(
      const std::vector<protocol::tile_blob>& tiles, int screen_w,
      int screen_h, bool& keyframe);
  /// True when AV1 should carry the entire picture instead of only patches.
  bool quality_av1_full_frame() const {
    av1_encoding_settings settings;
    {
      std::lock_guard<std::mutex> lock(av1_settings_mutex_);
      settings = av1_settings_;
    }
    return settings.full_frame;
  }
  int quality_av1_max_area_percent() const {
    av1_encoding_settings settings;
    {
      std::lock_guard<std::mutex> lock(av1_settings_mutex_);
      settings = av1_settings_;
    }
    if (settings.max_area_percent >= 1 &&
        settings.max_area_percent <= 100) {
      return settings.max_area_percent;
    }
    return quality_profile_for(quality_.load()).av1_max_area_percent;
  }

  bool running() const override { return running_.load(); }
  virtual capture_stats stats() const override;
  bool tile_clock(std::vector<uint8_t>& out) override;
  bool tile_seq(std::vector<uint8_t>& out) override;

  bool cursor(cursor_snapshot& out) const override;

  /// Keep at most one second of replayable frames, with independent count and
  /// byte bounds so high capture rates or large keyframes cannot grow memory.
  static constexpr uint64_t kHistoryMaxAgeUs = 1000000ULL;
  static constexpr std::size_t kHistoryMaxBytes = 128u * 1024u * 1024u;
  static constexpr std::size_t kHistoryMaxFrames = 60;

 protected:
  static int clamp_fps(int fps) { return fps < 1 ? 1 : (fps > 60 ? 60 : fps); }

  void publish(std::shared_ptr<const captured_frame> frame);
  /// Records the CPU time spent building/encoding one frame (EMA, 1/8 weight).
  void note_build_time(uint64_t us);
  /// Updates the per-cell freshness grid from a published frame's tiles.
  void update_cell_clock(const captured_frame& frame);
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
  std::atomic<bool> av1_allowed_{false};
  /// EMA of the build+encode time, read by the session layer.
  std::atomic<uint64_t> build_time_us_{0};
  /// Extra AV1 QP on top of the preset when the encoder misses the frame
  /// budget; 0 means the preset's own quality.
  std::atomic<int> av1_qp_offset_{0};
  /// Halve large AV1 rectangles while the encoder cannot meet the budget.
  std::atomic<bool> av1_downscale_{false};
  /// Keeps a video in full-screen AV1 mode for a moment after it pauses.
  uint64_t av1_full_screen_until_us_ = 0;
  /// Tile <-> AV1 stream hand-over, fed once per frame.
  stream_policy stream_policy_;
  std::shared_ptr<av1_stream_encoder> stream_encoder_;
  /// Codec the live stream encoder produces; a viewer set that changes which
  /// codec is decodable rebuilds the encoder.
  video_codec video_codec_ = video_codec::av1_stream;
  std::atomic<bool> av1_stream_allowed_{false};
  std::atomic<bool> h264_stream_allowed_{false};
  std::atomic<bool> stream_keyframe_pending_{true};
  /// A viewer is behind; box-average large tiles and stop lifting the AV1
  /// downscale while it lasts.
  std::atomic<bool> viewer_pressure_{false};
  std::atomic<bool> tile_downscale_{false};
  /// State for the "give AV1 up, send tiles" decision. Only the capture
  /// thread touches it; a preset change asks for a reset.
  av1_budget_policy av1_budget_;
  std::atomic<bool> av1_budget_reset_{false};
  /// Forced keyframe cadence; 0 means the backends' 30 s safety refresh only.
  std::atomic<int> periodic_keyframe_ms_{120000};
  /// Settings arrive from the API thread; the capture thread copies them.
  mutable std::mutex av1_settings_mutex_;
  av1_encoding_settings av1_settings_;
  /// Created lazily on the capture thread the first time AV1 is allowed.
  std::shared_ptr<av1_encoder> av1_encoder_;
  /// The configuration the current encoder (or failed attempt) was built for.
  bool encoder_configured_ = false;
  std::string encoder_device_;
  std::string encoder_speed_;
  int encoder_threads_ = 0;
  /// Newest capture timestamp per 128 px cell, protected by mutex_.
  std::vector<uint64_t> cell_clock_;
  /// Newest capture sequence per 128 px cell: the exact "you have this cell up
  /// to frame N" reference clients acknowledge against.
  std::vector<uint32_t> cell_seq_;
  uint16_t clock_cols_ = 0;
  uint16_t clock_rows_ = 0;
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
