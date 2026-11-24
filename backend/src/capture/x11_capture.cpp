#include "capture/capture.h"

#if MOLINK_HAVE_X11

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture/display_layout.h"
#include "capture/frame_encode.h"
#include "capture/frame_schedule.h"
#include "diag_log.h"
#include "protocol/frame_packet.h"

namespace molink {
namespace {

uint64_t epoch_us() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(system_clock::now().time_since_epoch())
          .count());
}

int ignore_x_error(Display*, XErrorEvent*) { return 0; }

uint32_t swap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0x0000ff00u) | ((v << 8) & 0x00ff0000u) |
         (v << 24);
}

/// XFixesCursorImage. As with XDamage, the runtime library is present but the
/// development headers often are not.
struct xfixes_cursor_image {
  short x;
  short y;
  unsigned short width;
  unsigned short height;
  unsigned short xhot;
  unsigned short yhot;
  unsigned long cursor_serial;
  unsigned long* pixels;
  unsigned long atom_name;
  unsigned long cursor_name;
};

/// XRRMonitorInfo from <X11/extensions/Xrandr.h>. The runtime library is
/// ubiquitous but the development headers are not always installed, so the
/// two entry points are resolved dynamically and the layout is declared here.
struct xrr_monitor_info {
  Atom name;
  Bool primary;
  Bool automatic;
  int noutput;
  int x;
  int y;
  int width;
  int height;
  int mwidth;
  int mheight;
  unsigned long* outputs;  // RROutput*
};

uint8_t extract_channel(uint32_t pixel, unsigned long mask) {
  if (mask == 0) return 0;
  unsigned shift = 0;
  while (shift < 32 && ((mask >> shift) & 1u) == 0) ++shift;
  const uint32_t field = mask >> shift;
  const uint32_t value = (pixel & mask) >> shift;
  if (field == 0xffu) return static_cast<uint8_t>(value);
  return static_cast<uint8_t>((value * 255u + field / 2u) / field);
}

/// Convert an XImage to tightly packed BGRA, top-down.
bool convert_to_bgra(XImage* image, std::vector<uint8_t>& out,
                     std::string& err) {
  const int width = image->width;
  const int height = image->height;
  if (width <= 0 || height <= 0) {
    err = "X image has no pixels";
    return false;
  }

  const int bpp = image->bits_per_pixel;
  const bool little_endian = image->byte_order == LSBFirst;
  const unsigned long rmask = image->red_mask ? image->red_mask : 0x00ff0000ul;
  const unsigned long gmask = image->green_mask ? image->green_mask : 0x0000ff00ul;
  const unsigned long bmask = image->blue_mask ? image->blue_mask : 0x000000fful;

  out.resize(static_cast<std::size_t>(width) * height * 4);

  // The common X11 case: depth 24 stored as 32-bit BGRA on little-endian.
  const bool packed_bgra = bpp == 32 && little_endian &&
                           bmask == 0x000000fful && gmask == 0x0000ff00ul &&
                           rmask == 0x00ff0000ul;
  if (packed_bgra) {
    for (int y = 0; y < height; ++y) {
      const uint8_t* src =
          reinterpret_cast<const uint8_t*>(image->data) +
          static_cast<std::size_t>(y) * image->bytes_per_line;
      uint8_t* dst = out.data() + static_cast<std::size_t>(y) * width * 4;
      for (int x = 0; x < width; ++x) {
        dst[x * 4 + 0] = src[x * 4 + 0];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 2];
        dst[x * 4 + 3] = 255;
      }
    }
    return true;
  }

  auto read_pixel = [bpp, little_endian](const uint8_t* p) -> uint32_t {
    if (bpp == 32) {
      uint32_t value = 0;
      std::memcpy(&value, p, 4);
      return little_endian ? value : swap32(value);
    }
    if (bpp == 24) {
      return little_endian
                 ? (static_cast<uint32_t>(p[0]) |
                    (static_cast<uint32_t>(p[1]) << 8) |
                    (static_cast<uint32_t>(p[2]) << 16))
                 : ((static_cast<uint32_t>(p[0]) << 16) |
                    (static_cast<uint32_t>(p[1]) << 8) |
                    static_cast<uint32_t>(p[2]));
    }
    if (bpp == 16) {
      return little_endian
                 ? (static_cast<uint32_t>(p[0]) |
                    (static_cast<uint32_t>(p[1]) << 8))
                 : ((static_cast<uint32_t>(p[0]) << 8) |
                    static_cast<uint32_t>(p[1]));
    }
    return 0;
  };

  const int bytes_per_pixel = (bpp + 7) / 8;
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = reinterpret_cast<const uint8_t*>(image->data) +
                         static_cast<std::size_t>(y) * image->bytes_per_line;
    uint8_t* dst = out.data() + static_cast<std::size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x) {
      const uint32_t pixel =
          read_pixel(row + static_cast<std::size_t>(x) * bytes_per_pixel);
      dst[x * 4 + 0] = extract_channel(pixel, bmask);
      dst[x * 4 + 1] = extract_channel(pixel, gmask);
      dst[x * 4 + 2] = extract_channel(pixel, rmask);
      dst[x * 4 + 3] = 255;
    }
  }
  return true;
}

/// XDamageNotifyEvent layout. libXdamage ships as a runtime library without
/// headers on many desktops, so the few pieces we need are declared here.
struct damage_notify_event {
  int type;
  unsigned long serial;
  int send_event;
  Display* display;
  Drawable drawable;
  unsigned long damage;
  int level;
  int more;
  unsigned long timestamp;
  XRectangle area;
  XRectangle geometry;
};

void union_rect(XRectangle& into, const XRectangle& r) {
  if (r.width == 0 || r.height == 0) return;
  if (into.width == 0 || into.height == 0) {
    into = r;
    return;
  }
  const int x0 = std::min<int>(into.x, r.x);
  const int y0 = std::min<int>(into.y, r.y);
  const int x1 = std::max<int>(into.x + into.width, r.x + r.width);
  const int y1 = std::max<int>(into.y + into.height, r.y + r.height);
  into.x = static_cast<short>(x0);
  into.y = static_cast<short>(y0);
  into.width = static_cast<unsigned short>(x1 - x0);
  into.height = static_cast<unsigned short>(y1 - y0);
}

class x11_capture final : public capture_base {
 public:
  explicit x11_capture(const capture_options& options) : capture_base(options.fps), options_(options) {
    // A self-pipe wakes the capture thread out of poll() when a viewer sends
    // input. Without it a key press that does not damage the screen waits out
    // the 200 ms poll timeout -- exactly the "sudden action" stall.
    if (::pipe(wake_fds_) == 0) {
      for (int& fd : wake_fds_) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
      }
    } else {
      wake_fds_[0] = -1;
      wake_fds_[1] = -1;
    }
  }
  ~x11_capture() override {
    stop();
    for (int& fd : wake_fds_) {
      if (fd >= 0) ::close(fd);
      fd = -1;
    }
  }

  void note_input(uint64_t input_us) override {
    capture_base::note_input(input_us);
    if (wake_fds_[1] < 0) return;
    const char byte = 1;
    // A full pipe already means "wake pending"; the byte is not data.
    const ssize_t ignored = ::write(wake_fds_[1], &byte, 1);
    (void)ignored;
  }

  bool start(std::string& err) override {
    if (running_.load()) return true;

    std::promise<std::string> ready;
    std::future<std::string> status = ready.get_future();
    running_.store(true);
    thread_ = std::thread([this, &ready] { thread_main(ready); });

    // XOpenDisplay can block or fail; do not report success until the capture
    // thread has a display and an image buffer.
    const std::string init_error = status.get();
    if (!init_error.empty()) {
      running_.store(false);
      if (thread_.joinable()) thread_.join();
      err = init_error;
      return false;
    }
    return true;
  }

  void stop() override {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
  }

  std::string backend() const override {
    return use_shm_.load() ? "x11-shm" : "x11";
  }
  bool is_real() const override { return true; }

  std::vector<display_info> displays() const override {
    std::lock_guard<std::mutex> lock(displays_mutex_);
    return displays_;
  }

  int current_display() const override { return display_index_.load(); }

  display_info current_display_info() const override {
    std::lock_guard<std::mutex> lock(displays_mutex_);
    const int index = display_index_.load();
    if (index >= 0 && index < static_cast<int>(displays_.size())) {
      return displays_[index];
    }
    return display_info{};
  }

  bool select_display(int index) override {
    std::lock_guard<std::mutex> lock(displays_mutex_);
    if (index < 0 || index >= static_cast<int>(displays_.size())) return false;
    requested_display_.store(index);
    return true;
  }

  bool copy_pixels(std::vector<uint8_t>& out, int& width,
                   int& height) override {
    std::lock_guard<std::recursive_mutex> lock(pixels_mutex_);
    if (frame_.empty() || width_ == 0 || height_ == 0) return false;
    out = frame_;
    width = width_;
    height = height_;
    return true;
  }

 private:
  void thread_main(std::promise<std::string>& ready) {
    std::string err;
    if (!open_display(err) || !setup_image(err)) {
      teardown();
      running_.store(false);
      ready.set_value(err.empty() ? "could not set up X11 capture" : err);
      return;
    }
    ready.set_value(std::string());

    // Damage gives prompt updates; this tick keeps the stream alive if the
    // extension is silent (a static screen, or a server that never reports).
    const auto safety_interval = std::chrono::milliseconds(500);
    // Absolute-deadline pacing: the period is the target interval, not the
    // interval plus this frame's encode time.
    const auto now_us = [] {
      return static_cast<int64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
    };
    const auto frame_interval_us = [this]() -> int64_t {
      return 1000000 / (std::max)(1, target_fps());
    };
    frame_schedule schedule;
    bool damage_mode = damage_ != 0;
    bool ever_damaged = false;
    std::string last_error;

    // Publish one keyframe immediately: it gives the welcome message and
    // /api/info a real screen size before any client asks for pixels.
    keyframe_requested_.store(true);
    trigger_ = capture_trigger::initial;
    trigger_us_ = epoch_us();
    capture_full(true, last_error);
    schedule.reset(now_us(), frame_interval_us());
    auto last_full = std::chrono::steady_clock::now();
    auto last_capture = std::chrono::steady_clock::now();
    auto next_display_poll = std::chrono::steady_clock::now();

    while (running_.load()) {
      // XRandR reports hotplug, rotation and mode changes live, but the root
      // geometry only changes for some of them (a rearrangement can leave it
      // identical). Compare the monitor list once a second so the clients hear
      // about every change, not only the ones that reach the damage path.
      const auto poll_now = std::chrono::steady_clock::now();
      if (poll_now >= next_display_poll) {
        next_display_poll = poll_now + std::chrono::seconds(1);
        poll_displays();
      }

      if (!active_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        schedule.reset(now_us(), frame_interval_us());
        last_capture = std::chrono::steady_clock::now();
        continue;
      }

      const int requested_display = requested_display_.exchange(-1);
      if (requested_display >= 0 &&
          requested_display != display_index_.load()) {
        std::string switch_error;
        if (apply_display(requested_display, switch_error)) {
          if (damage_ != 0 && damage_subtract_ != nullptr) {
            damage_subtract_(display_, damage_, 0, 0);
          }
          keyframe_requested_.store(true);
          last_full = std::chrono::steady_clock::now();
          last_capture = last_full;
          capture_full(true, switch_error);
          if (!switch_error.empty()) {
            std::fprintf(stderr, "molink capture: display switch: %s\n",
                         switch_error.c_str());
          }
        }
      }

      XRectangle dirty{};
      bool have_dirty = false;
      bool want_capture = false;
      update_cursor();

      capture_trigger trigger = capture_trigger::timer;
      uint64_t trigger_us = epoch_us();
      if (damage_mode) {
        struct pollfd descriptors[2];
        descriptors[0].fd = ConnectionNumber(display_);
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = wake_fds_[0];
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        // A static screen produces no damage event, and a 200 ms wait would
        // cap the AV1 stream at five frames a second. Poll briefly while the
        // stream is the active path so its cadence follows the frame rate.
        const int timeout =
            (stream_active() || input_priority_pending()) ? 5 : 200;
        ::poll(descriptors, 2, timeout);
        if (descriptors[1].revents & POLLIN) {
          // Input arrived: drain the byte(s) and let the checks below decide
          // whether the 30 ms gap allows a frame right now.
          char buffer[64];
          while (::read(wake_fds_[0], buffer, sizeof(buffer)) > 0) {
          }
        }
        // The event, if any, is observed now. Starting the trigger clock here
        // (before the rate limiter below) is what makes a capture that waits
        // for the next allowed slot look as slow as it feels.
        trigger_us = epoch_us();

        if (drain_damage_events(dirty)) {
          if (!ever_damaged) {
            std::fprintf(stderr, "molink capture: damage tracking active\n");
            ever_damaged = true;
          }
          have_dirty = true;
          want_capture = true;
          trigger = capture_trigger::change;
        }
        if (keyframe_requested_.load()) {
          want_capture = true;
          if (trigger != capture_trigger::change) {
            trigger = capture_trigger::keyframe;
          }
        }
        if (std::chrono::steady_clock::now() - last_capture >=
            safety_interval) {
          want_capture = true;
        }
        // The AV1 stream keeps a steady cadence; a static screen still has to
        // produce its (tiny) P frame every tick.
        if (stream_active()) {
          want_capture = true;
          if (trigger == capture_trigger::timer) trigger = capture_trigger::stream;
        }
      } else {
        want_capture = true;
      }
      // Input may start a frame even when the desktop reported no damage (a
      // key press that opens a menu, a click that only repaints later). The
      // 30 ms gap keeps a continuous drag from ignoring the target rate
      // entirely, and viewer/transport pressure vetoes it below.
      const bool input_ready = input_capture_ready(last_capture);
      if (input_ready) want_capture = true;
      if (!want_capture) continue;

      // A target-rate change re-anchors the grid; keeping the old deadline
      // would otherwise mix two intervals on the same schedule.
      const int64_t wanted_interval = frame_interval_us();
      if (wanted_interval != schedule.interval()) {
        schedule.set_interval(now_us(), wanted_interval);
      }
      // Rate limit, but let a pending input shorten the wait to the minimum
      // gap instead of the full target interval (200 ms at 5 fps).
      if (!input_ready) {
        auto deadline = std::chrono::steady_clock::time_point(
            std::chrono::microseconds(schedule.next()));
        const auto input_deadline = last_capture + kInputCaptureGap;
        if (pending_input_us() != 0 && input_deadline < deadline) {
          deadline = input_deadline;
        }
        while (std::chrono::steady_clock::now() < deadline) {
          wait_until(deadline);
          if (input_capture_ready(last_capture)) break;
        }
      }
      // The grab is about to start; the gap from trigger_us is the scheduling
      // delay this frame paid (rate limit, a blocked loop, a slow grab).
      if (input_capture_ready(last_capture)) {
        note_input_trigger(trigger_us, trigger);
      }
      trigger_ = trigger;
      trigger_us_ = trigger_us;

      std::string grab_error;
      const bool key = keyframe_requested_.exchange(false) || !have_previous_;
      // Periodic full refresh: it bounds any drift caused by a damage event we
      // did not see, and gives late joiners a recent keyframe. The host can
      // shorten it to as little as 1 s from the display settings.
      const int interval_ms =
          periodic_keyframe_ms() > 0 ? periodic_keyframe_ms() : 120000;
      const bool refresh_due = std::chrono::steady_clock::now() - last_full >
                               std::chrono::milliseconds(interval_ms);
      const bool whole_screen =
          key || refresh_due || !have_dirty ||
          static_cast<long>(dirty.width) * dirty.height >
              static_cast<long>(width_) * height_ / 2;

      bool ok = false;
      if (whole_screen) {
        ok = capture_full(key || refresh_due, grab_error);
        if (ok) last_full = std::chrono::steady_clock::now();
      } else {
        ok = capture_rect(dirty, grab_error);
      }

      if (ok) {
        if (!last_error.empty()) {
          std::fprintf(stderr, "molink capture: recovered\n");
          last_error.clear();
        }
      } else if (!grab_error.empty() && grab_error != last_error) {
        // Log transitions, not every skipped frame.
        std::fprintf(stderr, "molink capture: %s\n", grab_error.c_str());
        last_error = grab_error;
      }

      // Advance the absolute deadline by one interval. If this grab overran
      // more than a full interval, note_capture restarts the grid at "now"
      // instead of replaying the missed slots, so the backlog never grows.
      schedule.note_capture(now_us(), /*reanchor=*/input_ready);
      last_capture = std::chrono::steady_clock::now();
    }

    teardown();
  }

  bool open_display(std::string& err) {
    const char* requested = std::getenv("MOLINK_DISPLAY");
    display_ = XOpenDisplay(requested);
    if (display_ == nullptr) {
      err = requested != nullptr
                ? std::string("cannot open X display ") + requested
                : std::string("cannot open X display (DISPLAY is unset?)");
      return false;
    }
    XSetErrorHandler(&ignore_x_error);

    screen_ = DefaultScreen(display_);
    root_ = RootWindow(display_, screen_);
    visual_ = DefaultVisual(display_, screen_);
    depth_ = DefaultDepth(display_, screen_);
    use_shm_.store(XShmQueryExtension(display_) != 0);
    root_width_ = DisplayWidth(display_, screen_);
    root_height_ = DisplayHeight(display_, screen_);
    width_ = static_cast<uint16_t>(root_width_);
    height_ = static_cast<uint16_t>(root_height_);
    load_xrandr();
    enumerate_displays();
    const int wanted =
        options_.display_index >= 0 &&
                options_.display_index < static_cast<int>(displays_.size())
            ? options_.display_index
            : 0;
    std::string apply_error;
    apply_display(wanted, apply_error);
    setup_damage();  // best effort; fixed-rate capture works without it
    setup_cursor();
    return width_ > 0 && height_ > 0;
  }

  void load_xrandr() {
    xrandr_lib_ = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
    if (xrandr_lib_ == nullptr) {
      xrandr_lib_ = dlopen("libXrandr.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (xrandr_lib_ == nullptr) return;
    get_monitors_ = reinterpret_cast<decltype(get_monitors_)>(
        dlsym(xrandr_lib_, "XRRGetMonitors"));
    free_monitors_ = reinterpret_cast<decltype(free_monitors_)>(
        dlsym(xrandr_lib_, "XRRFreeMonitors"));
    if (get_monitors_ == nullptr || free_monitors_ == nullptr) {
      dlclose(xrandr_lib_);
      xrandr_lib_ = nullptr;
      get_monitors_ = nullptr;
      free_monitors_ = nullptr;
    }
  }

  struct display_probe {
    std::vector<display_info> displays;
    std::vector<display_layout_entry> layout;
  };

  /// Reads the monitor list without touching the published state, so a
  /// comparison can run cheaply and a failed probe leaves the old list alone.
  display_probe probe_displays() {
    display_probe probe;
    std::vector<display_info>& list = probe.displays;
    if (get_monitors_ != nullptr && display_ != nullptr && root_ != 0) {
      int count = 0;
      xrr_monitor_info* monitors =
          get_monitors_(display_, root_, True, &count);
      if (monitors != nullptr) {
        for (int i = 0; i < count; ++i) {
          const xrr_monitor_info& source = monitors[i];
          if (source.width <= 0 || source.height <= 0 ||
              source.width > 65535 || source.height > 65535) {
            continue;
          }
          display_info info;
          info.index = static_cast<int>(list.size());
          info.x = source.x;
          info.y = source.y;
          info.width = static_cast<uint16_t>(source.width);
          info.height = static_cast<uint16_t>(source.height);
          info.input_width = source.width;
          info.input_height = source.height;
          info.primary = source.primary != 0;
          char* atom = XGetAtomName(display_, source.name);
          if (atom != nullptr) {
            info.name = atom;
            XFree(atom);
          }
          if (info.name.empty()) {
            info.name = "Display " + std::to_string(info.index);
          }
          list.push_back(std::move(info));
        }
        free_monitors_(monitors);
      }
    }

    if (list.empty()) {
      display_info info;
      info.name = "Display 0";
      info.width = static_cast<uint16_t>(root_width_);
      info.height = static_cast<uint16_t>(root_height_);
      info.input_width = root_width_;
      info.input_height = root_height_;
      info.primary = true;
      list.push_back(std::move(info));
    }

    // Default index 0 is the primary monitor, then left-to-right/top-to-bottom.
    std::stable_sort(list.begin(), list.end(),
                     [](const display_info& a, const display_info& b) {
                       if (a.primary != b.primary) return a.primary;
                       if (a.y != b.y) return a.y < b.y;
                       return a.x < b.x;
                     });
    for (std::size_t i = 0; i < list.size(); ++i) {
      list[i].index = static_cast<int>(i);
      display_layout_entry entry;
      entry.name = list[i].name;
      entry.x = list[i].x;
      entry.y = list[i].y;
      entry.width = list[i].width;
      entry.height = list[i].height;
      entry.primary = list[i].primary;
      probe.layout.push_back(std::move(entry));
    }
    return probe;
  }

  void enumerate_displays() {
    display_probe probe = probe_displays();
    std::lock_guard<std::mutex> lock(displays_mutex_);
    displays_ = std::move(probe.displays);
    display_layout_ = std::move(probe.layout);
    have_layout_ = true;
  }

  /// Re-enumerates the monitors and, when the list differs from the published
  /// one, applies the new layout and records a notice for the session layer.
  /// Returns true when a change was applied.
  bool sync_displays(std::string& err) {
    if (display_ == nullptr) return false;
    display_probe probe = probe_displays();
    display_change_reason reason = display_change_reason::none;
    std::string previous_name;
    {
      std::lock_guard<std::mutex> lock(displays_mutex_);
      reason = have_layout_
                   ? compare_display_layouts(display_layout_, probe.layout)
                   : display_change_reason::none;
      const int previous_index = display_index_.load();
      if (previous_index >= 0 &&
          previous_index < static_cast<int>(displays_.size())) {
        previous_name = displays_[previous_index].name;
      }
      if (reason == display_change_reason::none) {
        // First observation: publish the list but do not call it a change.
        if (!have_layout_) {
          displays_ = probe.displays;
          display_layout_ = probe.layout;
          have_layout_ = true;
        }
        return false;
      }
      displays_ = probe.displays;
      display_layout_ = probe.layout;
    }

    // Keep capturing the same panel when it still exists; otherwise fall to
    // the primary one, which is what the user expects after unplugging.
    int wanted = find_display_by_name(probe.layout, previous_name);
    if (wanted < 0) wanted = primary_display_index(probe.layout);
    if (wanted < 0) wanted = 0;

    std::string apply_error;
    if (!apply_display(wanted, apply_error)) {
      err = apply_error.empty() ? "display change could not be applied"
                                : apply_error;
    }
    if (damage_ != 0 && damage_subtract_ != nullptr) {
      damage_subtract_(display_, damage_, 0, 0);
    }
    keyframe_requested_.store(true);

    const display_info& current = probe.displays[wanted];
    display_change_notice notice;
    notice.reason = reason;
    notice.index = wanted;
    notice.width = current.width;
    notice.height = current.height;
    notice.primary = current.primary;
    notice.displays = probe.displays;
    note_display_change(std::move(notice));

    int primary = -1;
    for (std::size_t i = 0; i < probe.displays.size(); ++i) {
      if (probe.displays[i].primary) {
        primary = static_cast<int>(i);
        break;
      }
    }
    diag().log(diag_level::info, "display.changed", std::string(),
               {{"count", std::to_string(probe.displays.size())},
                {"index", std::to_string(wanted)},
                {"primary", std::to_string(primary)},
                {"w", std::to_string(current.width)},
                {"h", std::to_string(current.height)},
                {"reason", display_change_reason_name(reason)}});
    return true;
  }

  /// Once-a-second cheap check. refresh_geometry() only reacts to a root size
  /// change; a rearrangement or a disabled monitor can leave the root alone.
  void poll_displays() {
    std::string err;
    if (!refresh_geometry(err)) return;
    std::string sync_error;
    sync_displays(sync_error);
    if (!sync_error.empty() && sync_error != last_display_error_) {
      std::fprintf(stderr, "molink capture: display change: %s\n",
                   sync_error.c_str());
      last_display_error_ = sync_error;
    }
  }

  bool apply_display(int index, std::string& err) {
    std::lock_guard<std::mutex> lock(displays_mutex_);
    if (index < 0 || index >= static_cast<int>(displays_.size())) return false;
    const display_info& info = displays_[index];
    display_index_.store(index);
    display_x_ = info.x;
    display_y_ = info.y;
    width_ = info.width;
    height_ = info.height;
    frame_.clear();
    previous_.clear();
    have_previous_ = false;
    return setup_image(err);
  }

  /// libXdamage ships as a runtime library on most desktops but its headers
  /// are in a -dev package. Resolve the three entry points dynamically so the
  /// build does not depend on that package being installed.
  bool setup_damage() {
    void* lib = dlopen("libXdamage.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) return false;

    auto query = reinterpret_cast<int (*)(Display*, int*, int*)>(
        dlsym(lib, "XDamageQueryExtension"));
    auto create = reinterpret_cast<DamageCreateFn>(dlsym(lib, "XDamageCreate"));
    auto destroy = reinterpret_cast<DamageDestroyFn>(dlsym(lib, "XDamageDestroy"));
    auto subtract =
        reinterpret_cast<DamageSubtractFn>(dlsym(lib, "XDamageSubtract"));
    if (query == nullptr || create == nullptr || destroy == nullptr ||
        subtract == nullptr) {
      dlclose(lib);
      return false;
    }

    int event_base = 0;
    int error_base = 0;
    if (!query(display_, &event_base, &error_base)) {
      dlclose(lib);
      return false;
    }

    // 2 == XDamageReportBoundingBox: each event carries a rectangle covering
    // everything damaged since the last subtract.
    const DamageId id = create(display_, root_, 2);
    if (id == 0) {
      dlclose(lib);
      return false;
    }

    damage_lib_ = lib;
    damage_create_ = create;
    damage_destroy_ = destroy;
    damage_subtract_ = subtract;
    damage_ = id;
    damage_event_base_ = event_base;
    return true;
  }

  /// libXfixes, resolved dynamically for the same reason as libXdamage.
  void setup_cursor() {
    void* lib = dlopen("libXfixes.so.3", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) return;
    auto query = reinterpret_cast<int (*)(Display*, int*, int*)>(
        dlsym(lib, "XFixesQueryExtension"));
    auto get = reinterpret_cast<xfixes_cursor_image* (*)(Display*)>(
        dlsym(lib, "XFixesGetCursorImage"));
    int event_base = 0;
    int error_base = 0;
    if (query == nullptr || get == nullptr ||
        !query(display_, &event_base, &error_base)) {
      dlclose(lib);
      return;
    }
    xfixes_lib_ = lib;
    get_cursor_image_ = get;
    xfixes_available_ = true;
  }

  void teardown_cursor() {
    if (xfixes_lib_ != nullptr) {
      dlclose(xfixes_lib_);
      xfixes_lib_ = nullptr;
    }
    get_cursor_image_ = nullptr;
    xfixes_available_ = false;
  }

  void update_cursor() {
    if (!xfixes_available_) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_cursor_query_ < std::chrono::milliseconds(100)) return;
    last_cursor_query_ = now;

    xfixes_cursor_image* image = get_cursor_image_(display_);
    if (image == nullptr) return;

    cursor_snapshot snapshot;
    snapshot.serial = static_cast<uint32_t>(image->cursor_serial);
    snapshot.width = image->width;
    snapshot.height = image->height;
    snapshot.hotspot_x = image->xhot;
    snapshot.hotspot_y = image->yhot;
    snapshot.x = image->x - display_x_;
    snapshot.y = image->y - display_y_;
    snapshot.valid = snapshot.x >= 0 && snapshot.y >= 0 &&
                     snapshot.x < width_ && snapshot.y < height_;
    snapshot.rgba.resize(static_cast<std::size_t>(image->width) * image->height * 4);
    for (std::size_t i = 0; i < static_cast<std::size_t>(image->width) * image->height; ++i) {
      const unsigned long pixel = image->pixels[i];  // ARGB
      snapshot.rgba[i * 4 + 0] = static_cast<uint8_t>((pixel >> 16) & 0xff);
      snapshot.rgba[i * 4 + 1] = static_cast<uint8_t>((pixel >> 8) & 0xff);
      snapshot.rgba[i * 4 + 2] = static_cast<uint8_t>(pixel & 0xff);
      snapshot.rgba[i * 4 + 3] = static_cast<uint8_t>((pixel >> 24) & 0xff);
    }
    XFree(image);
    set_cursor(std::move(snapshot));
  }

  bool drain_damage_events(XRectangle& dirty) {
    bool damaged = false;
    while (XPending(display_) > 0) {
      XEvent event;
      XNextEvent(display_, &event);
      if (event.type != damage_event_base_) continue;
      auto* notify = reinterpret_cast<damage_notify_event*>(&event);
      union_rect(dirty, notify->area);
      damaged = true;
    }
    if (damaged) {
      damage_subtract_(display_, damage_, 0, 0);
      // A rectangle outside the screen means the layout changed under us;
      // fall back to a full grab for this round.
      if (dirty.x < 0 || dirty.y < 0 ||
          static_cast<int>(dirty.x) + dirty.width > root_width_ ||
          static_cast<int>(dirty.y) + dirty.height > root_height_ ||
          dirty.width == 0 || dirty.height == 0) {
        dirty.x = 0;
        dirty.y = 0;
        dirty.width = static_cast<unsigned short>(root_width_);
        dirty.height = static_cast<unsigned short>(root_height_);
      }
    }
    return damaged;
  }

  void destroy_damage_object() {
    if (damage_ != 0 && damage_destroy_ != nullptr && display_ != nullptr) {
      damage_destroy_(display_, damage_);
    }
    damage_ = 0;
  }

  void unload_damage_library() {
    if (damage_lib_ != nullptr) {
      dlclose(damage_lib_);
      damage_lib_ = nullptr;
    }
    damage_create_ = nullptr;
    damage_destroy_ = nullptr;
    damage_subtract_ = nullptr;
    damage_event_base_ = -1;
  }

  bool setup_image(std::string& err) {
    teardown_image();
    have_previous_ = false;
    if (!use_shm_.load()) return true;

    image_ = XShmCreateImage(display_, visual_, depth_, ZPixmap, nullptr,
                             &shminfo_, width_, height_);
    if (image_ == nullptr) {
      use_shm_.store(false);
      err = "XShmCreateImage failed";
      return true;  // fall back to XGetImage
    }

    shminfo_.shmid = shmget(
        IPC_PRIVATE,
        static_cast<std::size_t>(image_->bytes_per_line) * image_->height,
        IPC_CREAT | 0600);
    if (shminfo_.shmid < 0) {
      teardown_image();
      use_shm_.store(false);
      err = "shmget failed";
      return true;
    }

    shminfo_.shmaddr = static_cast<char*>(shmat(shminfo_.shmid, nullptr, 0));
    if (shminfo_.shmaddr == reinterpret_cast<char*>(-1)) {
      shminfo_.shmaddr = nullptr;
      teardown_image();
      use_shm_.store(false);
      err = "shmat failed";
      return true;
    }

    shminfo_.readOnly = False;
    image_->data = shminfo_.shmaddr;
    if (!XShmAttach(display_, &shminfo_)) {
      teardown_image();
      use_shm_.store(false);
      err = "XShmAttach failed";
      return true;
    }
    XSync(display_, False);
    // The segment is released when the last attachment goes away.
    shmctl(shminfo_.shmid, IPC_RMID, nullptr);
    shm_attached_ = true;
    return true;
  }

  void teardown_image() {
    if (image_ != nullptr) {
      if (shm_attached_) {
        XShmDetach(display_, &shminfo_);
        XSync(display_, False);
        shm_attached_ = false;
      }
      // For the SHM path the data pointer belongs to the segment, not to
      // malloc; clearing it keeps XDestroyImage from freeing it.
      image_->data = nullptr;
      XDestroyImage(image_);
      image_ = nullptr;
    }
    if (shminfo_.shmaddr != nullptr) {
      shmdt(shminfo_.shmaddr);
      shminfo_.shmaddr = nullptr;
    }
    shminfo_.shmid = -1;
  }

  void teardown() {
    teardown_image();
    // Xlib keeps extension close-down hooks inside the dlopen'd libraries.
    // Destroy the damage object and close the display before unloading any of
    // them; doing it the other way round makes XCloseDisplay jump into an
    // unmapped module at exit.
    destroy_damage_object();
    if (display_ != nullptr) {
      XCloseDisplay(display_);
      display_ = nullptr;
    }
    unload_damage_library();
    teardown_cursor();
    if (xrandr_lib_ != nullptr) {
      dlclose(xrandr_lib_);
      xrandr_lib_ = nullptr;
      get_monitors_ = nullptr;
      free_monitors_ = nullptr;
    }
  }

  bool refresh_geometry(std::string& err) {
    XWindowAttributes attrs;
    if (display_ == nullptr || !XGetWindowAttributes(display_, root_, &attrs)) {
      err = "XGetWindowAttributes failed";
      return false;
    }
    if (attrs.width <= 0 || attrs.height <= 0) {
      err = "root window has no area";
      return false;
    }
    if (attrs.width != root_width_ || attrs.height != root_height_) {
      root_width_ = attrs.width;
      root_height_ = attrs.height;
      std::string sync_error;
      sync_displays(sync_error);
      if (!sync_error.empty()) {
        err = sync_error;
        return false;
      }
    }
    return true;
  }

  bool grab_full(std::vector<uint8_t>& out, std::string& err) {
    if (use_shm_.load() && image_ != nullptr &&
        XShmGetImage(display_, root_, image_, display_x_, display_y_,
                     AllPlanes)) {
      return convert_to_bgra(image_, out, err);
    }

    // The SHM segment can go stale when the screen is resized or the X server
    // restarts. Drop to the portable path rather than failing the stream.
    if (use_shm_.load()) {
      use_shm_.store(false);
      teardown_image();
    }

    XImage* image = XGetImage(display_, root_, display_x_, display_y_, width_,
                              height_, AllPlanes, ZPixmap);
    if (image == nullptr) {
      err = "XGetImage failed";
      return false;
    }
    const bool ok = convert_to_bgra(image, out, err);
    XDestroyImage(image);
    return ok;
  }

  bool grab_rect(const XRectangle& rect, std::vector<uint8_t>& out,
                 std::string& err) {
    if (rect.width == 0 || rect.height == 0) {
      err = "empty damage rectangle";
      return false;
    }
    XImage* image = XGetImage(display_, root_, display_x_ + rect.x,
                              display_y_ + rect.y, rect.width, rect.height,
                              AllPlanes, ZPixmap);
    if (image == nullptr) {
      err = "XGetImage(rect) failed";
      return false;
    }
    const bool ok = convert_to_bgra(image, out, err);
    XDestroyImage(image);
    return ok;
  }

  bool capture_full(bool keyframe, std::string& err) {
    std::lock_guard<std::recursive_mutex> pixels_lock(pixels_mutex_);
    if (!refresh_geometry(err)) return false;

    capture_start_us_ = epoch_us();
    std::vector<uint8_t> pixels;
    if (!grab_full(pixels, err)) return false;
    if (pixels.size() != static_cast<std::size_t>(width_) * height_ * 4) {
      err = "full grab returned an unexpected size";
      return false;
    }

    const bool size_changed = frame_.size() != pixels.size();
    frame_ = std::move(pixels);
    capture_done_us_ = epoch_us();
    note_grab(true, width_, height_);
    if (keyframe || size_changed || previous_.size() != frame_.size()) {
      previous_.assign(frame_.size(), 0);
      have_previous_ = false;  // every tile goes into the keyframe
    }
    return publish_frame(keyframe || size_changed, nullptr, err);
  }

  bool capture_rect(const XRectangle& rect, std::string& err) {
    std::lock_guard<std::recursive_mutex> pixels_lock(pixels_mutex_);
    if (!refresh_geometry(err)) return false;
    if (!have_previous_ ||
        frame_.size() != static_cast<std::size_t>(width_) * height_ * 4) {
      return capture_full(true, err);
    }

    // Damage rectangles are in root coordinates; translate them into the
    // selected monitor and drop anything that falls outside it.
    const int left = std::max<int>(rect.x, display_x_);
    const int top = std::max<int>(rect.y, display_y_);
    const int right =
        std::min<int>(rect.x + rect.width, display_x_ + width_);
    const int bottom =
        std::min<int>(rect.y + rect.height, display_y_ + height_);
    if (right <= left || bottom <= top) return true;
    const XRectangle local{static_cast<short>(left - display_x_),
                           static_cast<short>(top - display_y_),
                           static_cast<unsigned short>(right - left),
                           static_cast<unsigned short>(bottom - top)};

    std::vector<uint8_t> pixels;
    capture_start_us_ = epoch_us();
    if (!grab_rect(local, pixels, err)) return false;
    note_grab(false, local.width, local.height);

    const std::size_t stride = static_cast<std::size_t>(width_) * 4;
    for (uint16_t row = 0; row < local.height; ++row) {
      const std::size_t dst =
          static_cast<std::size_t>(local.y + row) * stride +
          static_cast<std::size_t>(local.x) * 4;
      std::memcpy(frame_.data() + dst,
                  pixels.data() + static_cast<std::size_t>(row) *
                                      local.width * 4,
                  static_cast<std::size_t>(local.width) * 4);
    }
    capture_done_us_ = epoch_us();
    return publish_frame(false, &local, err);
  }

  bool publish_frame(bool keyframe, const XRectangle* dirty, std::string& err) {
    const uint16_t tile = std::max<uint16_t>(
        64, options_.tile_size == 0 ? 256 : options_.tile_size);
    const std::size_t stride = static_cast<std::size_t>(width_) * 4;

    if (previous_.size() != frame_.size()) {
      previous_.assign(frame_.size(), 0);
      have_previous_ = false;
    }

    protocol::frame_header header;
    const uint32_t frame_seq = next_capture_seq();
    header.frame_seq = frame_seq;
    header.codec = static_cast<uint8_t>(protocol::codec::bgra);
    header.flags = keyframe ? protocol::kFrameFlagKeyframe
                            : protocol::kFrameFlagPartial;
    header.screen_w = width_;
    header.screen_h = height_;
    header.tile_w = tile;
    header.tile_h = tile;

    tile_dirty_rect dirty_rect;
    if (!keyframe && dirty != nullptr) {
      dirty_rect.x = dirty->x;
      dirty_rect.y = dirty->y;
      dirty_rect.width = dirty->width;
      dirty_rect.height = dirty->height;
      dirty_rect.valid = true;
    }

    const auto build_start = std::chrono::steady_clock::now();
    const uint64_t encode_start_us = epoch_us();
    std::vector<protocol::tile_blob> tiles;
    build_changed_tiles(frame_.data(), stride, width_, height_, tile,
                        previous_.data(), keyframe,
                        dirty_rect.valid ? &dirty_rect : nullptr,
                        /*force_opaque=*/false,
                        quality_change_threshold(options_.change_threshold),
                        tiles);

    encode_options encoding;
    encoding.compress = options_.compress && quality_compress();
    encoding.format = quality_format();
    encoding.effort = quality_effort();
    encoding.av1 = av1_for_frame();
    encoding.av1_qp = quality_av1_qp();
    encoding.av1_grayscale = quality_av1_grayscale();
    encoding.av1_downscale = quality_av1_downscale();
    encoding.tile_downscale = quality_tile_downscale();
    encoding.av1_max_area_percent = quality_av1_max_area_percent();
    encoding.av1_full_frame = quality_av1_full_frame();
    encoding.av1_full_screen_until_us = av1_full_screen_until();
    bool stream_keyframe = false;
    encoding.av1_stream = stream_for_frame(tiles, header.screen_w,
                                           header.screen_h, stream_keyframe);
    encoding.av1_stream_keyframe = stream_keyframe;
    encoding.av1_stream_qp = quality_av1_qp_stream();
    std::vector<uint8_t> av1_payload;
    bool av1_only = false;

    std::vector<uint8_t> payload;
    if (!encode_capture_frame(header, tiles, encoding, frame_.data(), stride, payload,
                           &av1_payload, err, &av1_only)) {
      return false;
    }
    const uint64_t encode_end_us = epoch_us();
    note_build_time(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - build_start)
            .count()));

    auto frame = std::make_shared<captured_frame>();
    frame->seq = frame_seq;
    frame->ts_us = epoch_us();
    frame->capture_start_us = capture_start_us_;
    frame->capture_done_us = capture_done_us_;
    frame->encode_start_us = encode_start_us;
    frame->encode_end_us = encode_end_us;
    frame->trigger = trigger_;
    frame->trigger_us = trigger_us_;
    frame->keyframe = keyframe;
    frame->width = width_;
    frame->height = height_;
    frame->tile_w = tile;
    frame->tile_h = tile;
    frame->codec = header.codec;
    frame->payload = std::move(payload);
    frame->av1_payload = std::move(av1_payload);
    frame->av1_only = av1_only;
    publish(std::move(frame));

    have_previous_ = true;
    return true;
  }

  capture_options options_;
  /// Pipeline stamps for the frame currently being built on this thread.
  uint64_t capture_start_us_ = 0;
  uint64_t capture_done_us_ = 0;
  uint64_t trigger_us_ = 0;
  capture_trigger trigger_ = capture_trigger::timer;
  int wake_fds_[2] = {-1, -1};

  Display* display_ = nullptr;
  int screen_ = 0;
  Window root_ = 0;
  Visual* visual_ = nullptr;
  int depth_ = 24;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  int root_width_ = 0;
  int root_height_ = 0;
  int display_x_ = 0;
  int display_y_ = 0;

  void* xrandr_lib_ = nullptr;
  xrr_monitor_info* (*get_monitors_)(Display*, Window, Bool, int*) = nullptr;
  void (*free_monitors_)(xrr_monitor_info*) = nullptr;
  mutable std::mutex displays_mutex_;
  std::vector<display_info> displays_;
  /// Comparison state for the once-a-second hotplug check. Only the capture
  /// thread touches it (displays_ is the snapshot other threads read).
  std::vector<display_layout_entry> display_layout_;
  bool have_layout_ = false;
  std::string last_display_error_;
  std::atomic<int> display_index_{0};
  std::atomic<int> requested_display_{-1};

  XImage* image_ = nullptr;
  XShmSegmentInfo shminfo_{};
  bool shm_attached_ = false;
  std::atomic<bool> use_shm_{false};

  using DamageId = unsigned long;
  using DamageCreateFn = DamageId (*)(Display*, Drawable, int);
  using DamageDestroyFn = void (*)(Display*, DamageId);
  using DamageSubtractFn = void (*)(Display*, DamageId, unsigned long,
                                    unsigned long);

  void* damage_lib_ = nullptr;
  DamageCreateFn damage_create_ = nullptr;
  DamageDestroyFn damage_destroy_ = nullptr;
  DamageSubtractFn damage_subtract_ = nullptr;
  DamageId damage_ = 0;
  int damage_event_base_ = -1;

  void* xfixes_lib_ = nullptr;
  xfixes_cursor_image* (*get_cursor_image_)(Display*) = nullptr;
  bool xfixes_available_ = false;
  std::chrono::steady_clock::time_point last_cursor_query_{};

  std::thread thread_;

  std::vector<uint8_t> frame_;
  mutable std::recursive_mutex pixels_mutex_;
  std::vector<uint8_t> previous_;
  bool have_previous_ = false;
};

}  // namespace

std::unique_ptr<capture_source> make_x11_capture(
    const capture_options& options) {
  return std::unique_ptr<capture_source>(new x11_capture(options));
}

}  // namespace molink

#else  // !MOLINK_HAVE_X11

namespace molink {

std::unique_ptr<capture_source> make_x11_capture(const capture_options&) {
  return nullptr;
}

}  // namespace molink

#endif
