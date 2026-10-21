#include "capture/capture.h"

#if MOLINK_HAVE_X11

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <dlfcn.h>
#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

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
  explicit x11_capture(const capture_options& options) : options_(options) {}
  ~x11_capture() override { stop(); }

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

    const int fps = std::max(1, options_.fps);
    const auto interval = std::chrono::milliseconds(1000 / fps);
    // Damage gives prompt updates; this tick keeps the stream alive if the
    // extension is silent (a static screen, or a server that never reports).
    const auto safety_interval = std::chrono::milliseconds(500);
    auto next_allowed = std::chrono::steady_clock::now();
    bool damage_mode = damage_ != 0;
    bool ever_damaged = false;
    std::string last_error;

    // Publish one keyframe immediately: it gives the welcome message and
    // /api/info a real screen size before any client asks for pixels.
    keyframe_requested_.store(true);
    capture_full(true, last_error);
    next_allowed = std::chrono::steady_clock::now() + interval;
    auto last_full = std::chrono::steady_clock::now();
    auto last_capture = std::chrono::steady_clock::now();

    while (running_.load()) {
      if (!active_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        next_allowed = std::chrono::steady_clock::now();
        last_capture = next_allowed;
        continue;
      }

      XRectangle dirty{};
      bool have_dirty = false;
      bool want_capture = false;

      if (damage_mode) {
        struct pollfd descriptor;
        descriptor.fd = ConnectionNumber(display_);
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        ::poll(&descriptor, 1, 200);

        if (drain_damage_events(dirty)) {
          if (!ever_damaged) {
            std::fprintf(stderr, "molink capture: damage tracking active\n");
            ever_damaged = true;
          }
          have_dirty = true;
          want_capture = true;
        }
        if (keyframe_requested_.load()) want_capture = true;
        if (std::chrono::steady_clock::now() - last_capture >=
            safety_interval) {
          want_capture = true;
        }
        if (!want_capture) continue;
      } else {
        want_capture = true;
      }
      if (!want_capture) continue;

      if (next_allowed > std::chrono::steady_clock::now()) {
        std::this_thread::sleep_until(next_allowed);
      }

      std::string grab_error;
      const bool key = keyframe_requested_.exchange(false) || !have_previous_;
      // Periodic full refresh: it bounds any drift caused by a damage event we
      // did not see, and gives late joiners a recent keyframe.
      const bool refresh_due = std::chrono::steady_clock::now() - last_full >
                               std::chrono::seconds(30);
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

      next_allowed = std::chrono::steady_clock::now() + interval;
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
    width_ = static_cast<uint16_t>(DisplayWidth(display_, screen_));
    height_ = static_cast<uint16_t>(DisplayHeight(display_, screen_));
    setup_damage();  // best effort; fixed-rate capture works without it
    return width_ > 0 && height_ > 0;
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
          static_cast<int>(dirty.x) + dirty.width > width_ ||
          static_cast<int>(dirty.y) + dirty.height > height_ ||
          dirty.width == 0 || dirty.height == 0) {
        dirty.x = 0;
        dirty.y = 0;
        dirty.width = width_;
        dirty.height = height_;
      }
    }
    return damaged;
  }

  void teardown_damage() {
    if (damage_ != 0 && damage_destroy_ != nullptr && display_ != nullptr) {
      damage_destroy_(display_, damage_);
    }
    damage_ = 0;
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
    teardown_damage();
    if (display_ != nullptr) {
      XCloseDisplay(display_);
      display_ = nullptr;
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
    if (attrs.width != width_ || attrs.height != height_) {
      width_ = static_cast<uint16_t>(attrs.width);
      height_ = static_cast<uint16_t>(attrs.height);
      setup_image(err);
    }
    return true;
  }

  bool grab_full(std::vector<uint8_t>& out, std::string& err) {
    if (use_shm_.load() && image_ != nullptr &&
        XShmGetImage(display_, root_, image_, 0, 0, AllPlanes)) {
      return convert_to_bgra(image_, out, err);
    }

    // The SHM segment can go stale when the screen is resized or the X server
    // restarts. Drop to the portable path rather than failing the stream.
    if (use_shm_.load()) {
      use_shm_.store(false);
      teardown_image();
    }

    XImage* image =
        XGetImage(display_, root_, 0, 0, width_, height_, AllPlanes, ZPixmap);
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
    XImage* image = XGetImage(display_, root_, rect.x, rect.y, rect.width,
                              rect.height, AllPlanes, ZPixmap);
    if (image == nullptr) {
      err = "XGetImage(rect) failed";
      return false;
    }
    const bool ok = convert_to_bgra(image, out, err);
    XDestroyImage(image);
    return ok;
  }

  bool capture_full(bool keyframe, std::string& err) {
    if (!refresh_geometry(err)) return false;

    std::vector<uint8_t> pixels;
    if (!grab_full(pixels, err)) return false;
    if (pixels.size() != static_cast<std::size_t>(width_) * height_ * 4) {
      err = "full grab returned an unexpected size";
      return false;
    }

    const bool size_changed = frame_.size() != pixels.size();
    frame_ = std::move(pixels);
    note_grab(true, width_, height_);
    if (keyframe || size_changed || previous_.size() != frame_.size()) {
      previous_.assign(frame_.size(), 0);
      have_previous_ = false;  // every tile goes into the keyframe
    }
    return publish_frame(keyframe || size_changed, nullptr, err);
  }

  bool capture_rect(const XRectangle& rect, std::string& err) {
    if (!refresh_geometry(err)) return false;
    if (!have_previous_ ||
        frame_.size() != static_cast<std::size_t>(width_) * height_ * 4) {
      return capture_full(true, err);
    }

    std::vector<uint8_t> pixels;
    if (!grab_rect(rect, pixels, err)) return false;
    note_grab(false, rect.width, rect.height);

    const std::size_t stride = static_cast<std::size_t>(width_) * 4;
    for (uint16_t row = 0; row < rect.height; ++row) {
      const std::size_t dst =
          static_cast<std::size_t>(rect.y + row) * stride +
          static_cast<std::size_t>(rect.x) * 4;
      std::memcpy(frame_.data() + dst,
                  pixels.data() + static_cast<std::size_t>(row) *
                                      rect.width * 4,
                  static_cast<std::size_t>(rect.width) * 4);
    }
    return publish_frame(false, &rect, err);
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
    header.frame_seq = seq_;
    header.codec = static_cast<uint8_t>(protocol::codec::bgra);
    header.flags = keyframe ? protocol::kFrameFlagKeyframe
                            : protocol::kFrameFlagPartial;
    header.screen_w = width_;
    header.screen_h = height_;
    header.tile_w = tile;
    header.tile_h = tile;

    std::vector<protocol::tile_blob> tiles;
    for (uint32_t ty = 0; ty < height_; ty += tile) {
      for (uint32_t tx = 0; tx < width_; tx += tile) {
        const uint16_t tw =
            static_cast<uint16_t>(std::min<uint32_t>(tile, width_ - tx));
        const uint16_t th =
            static_cast<uint16_t>(std::min<uint32_t>(tile, height_ - ty));

        if (!keyframe && dirty != nullptr) {
          const bool overlaps =
              static_cast<int>(tx) < static_cast<int>(dirty->x) + static_cast<int>(dirty->width) &&
              static_cast<int>(dirty->x) < static_cast<int>(tx) + static_cast<int>(tw) &&
              static_cast<int>(ty) < static_cast<int>(dirty->y) + static_cast<int>(dirty->height) &&
              static_cast<int>(dirty->y) < static_cast<int>(ty) + static_cast<int>(th);
          if (!overlaps) continue;
        }

        const std::size_t offset =
            static_cast<std::size_t>(ty) * stride +
            static_cast<std::size_t>(tx) * 4;

        bool changed = keyframe;
        if (!changed) {
          for (uint16_t row = 0; row < th; ++row) {
            const std::size_t at =
                offset + static_cast<std::size_t>(row) * stride;
            if (std::memcmp(frame_.data() + at, previous_.data() + at,
                            static_cast<std::size_t>(tw) * 4) != 0) {
              changed = true;
              break;
            }
          }
        }
        if (!changed) continue;

        protocol::tile_blob blob;
        blob.x = static_cast<uint16_t>(tx);
        blob.y = static_cast<uint16_t>(ty);
        blob.w = tw;
        blob.h = th;
        blob.flags = protocol::kTileFlagChanged |
                     (keyframe ? protocol::kTileFlagKeyframe : 0);
        blob.data.resize(static_cast<std::size_t>(tw) * th * 4);
        for (uint16_t row = 0; row < th; ++row) {
          const std::size_t at =
              offset + static_cast<std::size_t>(row) * stride;
          std::memcpy(blob.data.data() + static_cast<std::size_t>(row) * tw * 4,
                      frame_.data() + at, static_cast<std::size_t>(tw) * 4);
          // Keep the baseline in step with what this frame claims to be.
          std::memcpy(previous_.data() + at, frame_.data() + at,
                      static_cast<std::size_t>(tw) * 4);
        }
        tiles.push_back(std::move(blob));
      }
    }

    std::vector<uint8_t> payload;
    if (!protocol::build_frame_payload(header, tiles, payload, err)) {
      return false;
    }

    auto frame = std::make_shared<captured_frame>();
    frame->seq = seq_++;
    frame->ts_us = epoch_us();
    frame->keyframe = keyframe;
    frame->width = width_;
    frame->height = height_;
    frame->tile_w = tile;
    frame->tile_h = tile;
    frame->payload = std::move(payload);
    publish(std::move(frame));

    have_previous_ = true;
    return true;
  }

  capture_options options_;

  Display* display_ = nullptr;
  int screen_ = 0;
  Window root_ = 0;
  Visual* visual_ = nullptr;
  int depth_ = 24;
  uint16_t width_ = 0;
  uint16_t height_ = 0;

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

  std::thread thread_;

  std::vector<uint8_t> frame_;
  std::vector<uint8_t> previous_;
  bool have_previous_ = false;
  uint32_t seq_ = 0;
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
