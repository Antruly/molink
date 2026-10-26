#include "capture/capture.h"

#if defined(__APPLE__)

#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "capture/frame_encode.h"
#include "protocol/frame_packet.h"

namespace molink {
namespace {

uint64_t epoch_us() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(system_clock::now().time_since_epoch())
          .count());
}

class macos_capture final : public capture_base {
 public:
  explicit macos_capture(const capture_options& options) : capture_base(options.fps), options_(options) {}
  ~macos_capture() override { stop(); }

  bool start(std::string& err) override {
    if (running_.load()) return true;
    std::promise<std::string> ready;
    std::future<std::string> status = ready.get_future();
    running_.store(true);
    thread_ = std::thread([this, &ready] { thread_main(ready); });
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

  std::string backend() const override { return "coregraphics"; }
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

 private:
  void thread_main(std::promise<std::string>& ready) {
    // macOS 10.15+ gates screen capture behind a TCC permission.
    if (!CGPreflightScreenCaptureAccess()) {
      CGRequestScreenCaptureAccess();
    }
    if (!CGPreflightScreenCaptureAccess()) {
      ready.set_value(
          "screen recording permission is not granted (System Settings > "
          "Privacy & Security > Screen Recording)");
      running_.store(false);
      return;
    }

    if (!enumerate_displays()) {
      ready.set_value("no active display");
      running_.store(false);
      return;
    }
    int wanted = options_.display_index;
    if (wanted < 0 || wanted >= static_cast<int>(display_ids_.size())) {
      wanted = 0;
    }
    if (!apply_display(wanted)) {
      ready.set_value("could not select the display");
      running_.store(false);
      return;
    }
    ready.set_value(std::string());

    const int fps = target_fps();
    auto next_allowed = std::chrono::steady_clock::now();
    auto last_full = std::chrono::steady_clock::now();
    std::vector<uint8_t> pixels;

    while (running_.load()) {
      if (!active_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        next_allowed = std::chrono::steady_clock::now();
        continue;
      }

      const int requested = requested_display_.exchange(-1);
      if (requested >= 0 && requested != display_index_.load()) {
        if (apply_display(requested)) {
          keyframe_requested_.store(true);
          last_full = std::chrono::steady_clock::now();
        }
      }

      std::string grab_error;
      if (grab(pixels, grab_error)) {
        const bool key = keyframe_requested_.exchange(false) || !have_previous_;
        const bool refresh_due =
            std::chrono::steady_clock::now() - last_full > std::chrono::seconds(30);
        publish_frame(pixels, key || refresh_due);
        if (key || refresh_due) last_full = std::chrono::steady_clock::now();
      } else if (!grab_error.empty()) {
        std::fprintf(stderr, "molink capture: %s\n", grab_error.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }

      next_allowed += std::chrono::milliseconds(1000 / fps);
      const auto now = std::chrono::steady_clock::now();
      if (next_allowed > now) std::this_thread::sleep_until(next_allowed);
      else next_allowed = now;
    }
  }

  bool enumerate_displays() {
    uint32_t count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess ||
        count == 0) {
      return false;
    }
    std::vector<CGDirectDisplayID> ids(count);
    if (CGGetActiveDisplayList(count, ids.data(), &count) != kCGErrorSuccess) {
      return false;
    }
    ids.resize(count);
    const CGDirectDisplayID main_id = CGMainDisplayID();
    const auto main_it = std::find(ids.begin(), ids.end(), main_id);
    if (main_it != ids.end()) {
      std::rotate(ids.begin(), main_it, main_it + 1);
    }

    std::vector<display_info> list;
    std::vector<CGDirectDisplayID> kept;
    for (CGDirectDisplayID id : ids) {
      const std::size_t pixel_w = CGDisplayPixelsWide(id);
      const std::size_t pixel_h = CGDisplayPixelsHigh(id);
      if (pixel_w == 0 || pixel_h == 0 || pixel_w > 65535 ||
          pixel_h > 65535) {
        continue;
      }
      const CGRect bounds = CGDisplayBounds(id);
      display_info info;
      info.index = static_cast<int>(list.size());
      info.name = "Display " + std::to_string(info.index);
      info.x = static_cast<int>(std::lround(bounds.origin.x));
      info.y = static_cast<int>(std::lround(bounds.origin.y));
      info.width = static_cast<uint16_t>(pixel_w);
      info.height = static_cast<uint16_t>(pixel_h);
      info.input_width = static_cast<int>(std::lround(bounds.size.width));
      info.input_height = static_cast<int>(std::lround(bounds.size.height));
      info.primary = id == main_id;
      list.push_back(std::move(info));
      kept.push_back(id);
    }
    if (list.empty()) return false;

    std::lock_guard<std::mutex> lock(displays_mutex_);
    displays_ = std::move(list);
    display_ids_ = std::move(kept);
    return true;
  }

  bool apply_display(int index) {
    std::lock_guard<std::mutex> lock(displays_mutex_);
    if (index < 0 || index >= static_cast<int>(display_ids_.size())) {
      return false;
    }
    display_id_ = display_ids_[index];
    display_index_.store(index);
    width_ = displays_[index].width;
    height_ = displays_[index].height;
    previous_.clear();
    have_previous_ = false;
    return true;
  }

  bool grab(std::vector<uint8_t>& pixels, std::string& err) {
    // macOS 15 marks CGDisplayCreateImage unavailable in the SDK while the
    // symbol still exists. Resolve it at runtime so the build is not tied to a
    // deployment target and a future removal degrades to synthetic frames.
    using CreateImageFn = CGImageRef (*)(CGDirectDisplayID);
    static CreateImageFn create_image = reinterpret_cast<CreateImageFn>(
        dlsym(RTLD_DEFAULT, "CGDisplayCreateImage"));
    if (create_image == nullptr) {
      err = "CGDisplayCreateImage is unavailable on this system";
      return false;
    }
    CGImageRef image = create_image(display_id_);
    if (image == nullptr) {
      err = "screen capture returned null (permission revoked?)";
      return false;
    }
    const std::size_t width = CGImageGetWidth(image);
    const std::size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0) {
      CGImageRelease(image);
      err = "empty display image";
      return false;
    }
    if (width != width_ || height != height_) {
      width_ = static_cast<uint16_t>(width);
      height_ = static_cast<uint16_t>(height);
      have_previous_ = false;
    }

    pixels.resize(width * height * 4);
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        pixels.data(), width, height, 8, width * 4, color_space,
        kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
    if (context == nullptr || color_space == nullptr) {
      if (context != nullptr) CGContextRelease(context);
      if (color_space != nullptr) CGColorSpaceRelease(color_space);
      CGImageRelease(image);
      err = "CGBitmapContextCreate failed";
      return false;
    }
    CGContextDrawImage(context,
                       CGRectMake(0, 0, static_cast<CGFloat>(width),
                                  static_cast<CGFloat>(height)),
                       image);
    CGContextRelease(context);
    CGColorSpaceRelease(color_space);
    CGImageRelease(image);
    return true;
  }

  void publish_frame(std::vector<uint8_t>& pixels, bool keyframe) {
    const uint16_t tile = options_.tile_size == 0 ? 256 : options_.tile_size;
    const std::size_t stride = static_cast<std::size_t>(width_) * 4;

    if (previous_.size() != pixels.size()) {
      previous_.assign(pixels.size(), 0);
      have_previous_ = false;
      keyframe = true;
    }

    protocol::frame_header header;
    header.frame_seq = seq_;
    header.flags = keyframe ? protocol::kFrameFlagKeyframe
                            : protocol::kFrameFlagPartial;
    header.screen_w = width_;
    header.screen_h = height_;
    header.tile_w = tile;
    header.tile_h = tile;

    std::vector<protocol::tile_blob> tiles;
    build_changed_tiles(pixels.data(), stride, width_, height_, tile,
                        previous_.data(), keyframe, nullptr,
                        /*force_opaque=*/true,
                        quality_change_threshold(options_.change_threshold),
                        tiles);

    encode_options encoding;
    encoding.compress = options_.compress && quality_compress();
    encoding.format = quality_format();
    encoding.effort = quality_effort();
    encoding.av1 = av1_for_frame();
    encoding.av1_qp = quality_av1_qp();
    encoding.av1_grayscale = quality_av1_grayscale();
    std::vector<uint8_t> av1_payload;

    std::vector<uint8_t> payload;
    std::string err;
    if (!encode_capture_frame(header, tiles, encoding, pixels.data(), stride, payload,
                           &av1_payload, err)) {
      return;
    }

    auto frame = std::make_shared<captured_frame>();
    frame->seq = seq_++;
    frame->ts_us = epoch_us();
    frame->keyframe = keyframe;
    frame->width = width_;
    frame->height = height_;
    frame->tile_w = tile;
    frame->tile_h = tile;
    frame->codec = header.codec;
    frame->payload = std::move(payload);
    frame->av1_payload = std::move(av1_payload);
    publish(std::move(frame));
    have_previous_ = true;
  }

  capture_options options_;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  CGDirectDisplayID display_id_ = CGMainDisplayID();
  std::vector<display_info> displays_;
  std::vector<CGDirectDisplayID> display_ids_;
  mutable std::mutex displays_mutex_;
  std::atomic<int> display_index_{0};
  std::atomic<int> requested_display_{-1};
  std::vector<uint8_t> previous_;
  bool have_previous_ = false;
  uint32_t seq_ = 0;
  std::thread thread_;
};

}  // namespace

std::unique_ptr<capture_source> make_macos_capture(
    const capture_options& options) {
  return std::unique_ptr<capture_source>(new macos_capture(options));
}

}  // namespace molink

#else  // !__APPLE__

namespace molink {
std::unique_ptr<capture_source> make_macos_capture(const capture_options&) {
  return nullptr;
}
}  // namespace molink

#endif
