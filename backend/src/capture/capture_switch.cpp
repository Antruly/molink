#include "capture/capture_switch.h"

#include <utility>

namespace molink {

capture_switch::capture_switch(std::shared_ptr<capture_source> initial,
                               factory make, bool secure_desktop)
    : inner_(std::move(initial)),
      make_(std::move(make)),
      secure_(secure_desktop) {}

std::shared_ptr<capture_source> capture_switch::inner() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return inner_;
}

bool capture_switch::switch_desktop(bool secure_desktop) {
  if (make_ == nullptr) return false;
  std::shared_ptr<capture_source> next = make_(secure_desktop);
  if (next == nullptr) {
    // The new desktop has no backend (for example the helper is not running
    // for the secure desktop): keep what we have rather than going blank.
    return false;
  }
  // A secure-desktop source reports itself; trust the source over the caller
  // so input routing follows the same answer.
  const bool secure = next->is_secure_desktop();
  if (secure_desktop && !secure) {
    // The desktop is secure but the build fell back to a source that cannot
    // see it (the helper was busy): keeping the current picture and retrying
    // beats swapping to a black GDI/DXGI view of the invisible desktop.
    next->stop();
    return false;
  }
  std::shared_ptr<capture_source> previous;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    previous = std::move(inner_);
    inner_ = next;
    secure_.store(secure);
    replay_settings(*inner_);
  }
  if (previous != nullptr && previous != next) previous->stop();
  return true;
}

void capture_switch::replay_settings(capture_source& source) const {
  source.set_active(active_.load());
  if (target_fps_.load() > 0) source.set_target_fps(target_fps_.load());
  source.set_quality(static_cast<quality_mode>(quality_.load()));
  source.set_av1_allowed(av1_allowed_.load());
  source.set_keyframe_interval_ms(keyframe_interval_ms_.load());
  source.set_viewer_pressure(pressure_.load());
  source.set_av1_stream_allowed(av1_stream_allowed_.load());
  source.set_h264_stream_allowed(h264_stream_allowed_.load());
  source.set_vp9_stream_allowed(vp9_stream_allowed_.load());
  source.set_vp8_stream_allowed(vp8_stream_allowed_.load());
  source.set_stream_always(stream_always_.load());
  source.set_stream_enter_percent(stream_enter_percent_.load());
  // A forced-stream window outlives the swap: the secure desktop relies on it
  // from the moment the switch happens.
  const uint64_t now = monotonic_ms();
  const uint64_t until = stream_nudge_until_.load();
  if (until > now) source.request_stream(until - now);
  av1_encoding_settings settings;
  {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    settings = av1_settings_;
  }
  source.set_av1_settings(settings);
}

bool capture_switch::start(std::string& err) {
  const std::shared_ptr<capture_source> source = inner();
  if (source == nullptr) {
    err = "no capture source";
    return false;
  }
  return source->start(err);
}

void capture_switch::stop() { with([](capture_source& s) { s.stop(); }); }

bool capture_switch::running() const {
  bool result = false;
  with([&result](const capture_source& s) { result = s.running(); });
  return result;
}

std::string capture_switch::backend() const {
  std::string result;
  with([&result](const capture_source& s) { result = s.backend(); });
  return result;
}

tile_format capture_switch::frame_format() const {
  tile_format result = tile_format::bgra;
  with([&result](const capture_source& s) { result = s.frame_format(); });
  return result;
}

std::shared_ptr<const captured_frame> capture_switch::latest() const {
  std::shared_ptr<const captured_frame> result;
  with([&result](const capture_source& s) { result = s.latest(); });
  return result;
}

std::shared_ptr<const captured_frame> capture_switch::find(
    uint32_t seq) const {
  std::shared_ptr<const captured_frame> result;
  with([&seq, &result](const capture_source& s) { result = s.find(seq); });
  return result;
}

void capture_switch::request_keyframe() {
  with([](capture_source& s) { s.request_keyframe(); });
}

void capture_switch::set_active(bool active) {
  active_.store(active);
  with([active](capture_source& s) { s.set_active(active); });
}

void capture_switch::set_target_fps(int fps) {
  target_fps_.store(fps);
  with([fps](capture_source& s) { s.set_target_fps(fps); });
}

int capture_switch::target_fps() const {
  int result = 0;
  with([&result](const capture_source& s) { result = s.target_fps(); });
  return result;
}

uint64_t capture_switch::build_time_us() const {
  uint64_t result = 0;
  with([&result](const capture_source& s) { result = s.build_time_us(); });
  return result;
}

void capture_switch::set_quality(quality_mode mode) {
  quality_.store(static_cast<int>(mode));
  with([mode](capture_source& s) { s.set_quality(mode); });
}

quality_mode capture_switch::quality() const {
  quality_mode result = quality_mode::balanced;
  with([&result](const capture_source& s) { result = s.quality(); });
  return result;
}

void capture_switch::set_av1_allowed(bool allowed) {
  av1_allowed_.store(allowed);
  with([allowed](capture_source& s) { s.set_av1_allowed(allowed); });
}

void capture_switch::set_av1_settings(const av1_encoding_settings& settings) {
  {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    av1_settings_ = settings;
  }
  with([&settings](capture_source& s) { s.set_av1_settings(settings); });
}

void capture_switch::set_keyframe_interval_ms(int ms) {
  keyframe_interval_ms_.store(ms);
  with([ms](capture_source& s) { s.set_keyframe_interval_ms(ms); });
}

bool capture_switch::tile_clock(std::vector<uint8_t>& out) {
  bool result = false;
  with([&out, &result](capture_source& s) { result = s.tile_clock(out); });
  return result;
}

bool capture_switch::tile_seq(std::vector<uint8_t>& out) {
  bool result = false;
  with([&out, &result](capture_source& s) { result = s.tile_seq(out); });
  return result;
}

void capture_switch::set_viewer_pressure(bool pressure) {
  pressure_.store(pressure);
  with([pressure](capture_source& s) { s.set_viewer_pressure(pressure); });
}

void capture_switch::note_viewer_rate(double fps, int target_fps, bool demand,
                                      uint64_t now_ms) {
  with([&](capture_source& s) {
    s.note_viewer_rate(fps, target_fps, demand, now_ms);
  });
}

void capture_switch::set_av1_stream_allowed(bool allowed) {
  av1_stream_allowed_.store(allowed);
  with([allowed](capture_source& s) { s.set_av1_stream_allowed(allowed); });
}

void capture_switch::set_stream_enter_percent(int percent) {
  stream_enter_percent_.store(percent);
  with([percent](capture_source& s) { s.set_stream_enter_percent(percent); });
}

void capture_switch::set_h264_stream_allowed(bool allowed) {
  h264_stream_allowed_.store(allowed);
  with([allowed](capture_source& s) { s.set_h264_stream_allowed(allowed); });
}

void capture_switch::request_stream(uint64_t ms) {
  stream_nudge_until_.store(monotonic_ms() + ms);
  with([ms](capture_source& s) { s.request_stream(ms); });
}

bool capture_switch::stream_on() const {
  bool result = false;
  with([&result](const capture_source& s) { result = s.stream_on(); });
  return result;
}

void capture_switch::set_stream_always(bool always) {
  stream_always_.store(always);
  with([always](capture_source& s) { s.set_stream_always(always); });
}

void capture_switch::set_vp9_stream_allowed(bool allowed) {
  vp9_stream_allowed_.store(allowed);
  with([allowed](capture_source& s) { s.set_vp9_stream_allowed(allowed); });
}

void capture_switch::set_vp8_stream_allowed(bool allowed) {
  vp8_stream_allowed_.store(allowed);
  with([allowed](capture_source& s) { s.set_vp8_stream_allowed(allowed); });
}

std::vector<display_info> capture_switch::displays() const {
  std::vector<display_info> result;
  with([&result](const capture_source& s) { result = s.displays(); });
  return result;
}

int capture_switch::current_display() const {
  int result = 0;
  with([&result](const capture_source& s) { result = s.current_display(); });
  return result;
}

display_info capture_switch::current_display_info() const {
  display_info result;
  with([&result](const capture_source& s) {
    result = s.current_display_info();
  });
  return result;
}

bool capture_switch::select_display(int index) {
  bool result = index == 0;
  with([&index, &result](capture_source& s) {
    result = s.select_display(index);
  });
  return result;
}

bool capture_switch::set_display_mode(uint16_t width, uint16_t height) {
  bool result = false;
  with([&](capture_source& s) { result = s.set_display_mode(width, height); });
  return result;
}

bool capture_switch::supports_display_mode() const {
  bool result = false;
  with([&result](const capture_source& s) {
    result = s.supports_display_mode();
  });
  return result;
}

bool capture_switch::is_real() const {
  bool result = false;
  with([&result](const capture_source& s) { result = s.is_real(); });
  return result;
}

bool capture_switch::is_secure_desktop() const {
  bool result = false;
  with([&result](const capture_source& s) {
    result = s.is_secure_desktop();
  });
  return result;
}

capture_stats capture_switch::stats() const {
  capture_stats result;
  with([&result](const capture_source& s) { result = s.stats(); });
  return result;
}

bool capture_switch::cursor(cursor_snapshot& out) const {
  bool result = false;
  with([&out, &result](const capture_source& s) { result = s.cursor(out); });
  return result;
}

bool capture_switch::copy_pixels(std::vector<uint8_t>& out, int& width,
                                 int& height) {
  bool result = false;
  with([&](capture_source& s) {
    result = s.copy_pixels(out, width, height);
  });
  return result;
}

}  // namespace molink
