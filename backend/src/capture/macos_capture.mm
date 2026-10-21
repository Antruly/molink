#include "capture/capture.h"

#if defined(__APPLE__)

#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
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
  explicit macos_capture(const capture_options& options) : options_(options) {}
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

    const CGDirectDisplayID display = CGMainDisplayID();
    const std::size_t width = CGDisplayPixelsWide(display);
    const std::size_t height = CGDisplayPixelsHigh(display);
    if (width == 0 || height == 0 || width > 65535 || height > 65535) {
      ready.set_value("unexpected display size");
      running_.store(false);
      return;
    }
    width_ = static_cast<uint16_t>(width);
    height_ = static_cast<uint16_t>(height);
    ready.set_value(std::string());

    const int fps = options_.fps > 0 ? options_.fps : 15;
    auto next_allowed = std::chrono::steady_clock::now();
    auto last_full = std::chrono::steady_clock::now();
    std::vector<uint8_t> pixels;

    while (running_.load()) {
      if (!active_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        next_allowed = std::chrono::steady_clock::now();
        continue;
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

  bool grab(std::vector<uint8_t>& pixels, std::string& err) {
    CGImageRef image = CGDisplayCreateImage(CGMainDisplayID());
    if (image == nullptr) {
      err = "CGDisplayCreateImage returned null (permission revoked?)";
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
    for (uint32_t ty = 0; ty < height_; ty += tile) {
      for (uint32_t tx = 0; tx < width_; tx += tile) {
        const uint16_t tw =
            static_cast<uint16_t>(std::min<uint32_t>(tile, width_ - tx));
        const uint16_t th =
            static_cast<uint16_t>(std::min<uint32_t>(tile, height_ - ty));
        const std::size_t offset =
            static_cast<std::size_t>(ty) * stride +
            static_cast<std::size_t>(tx) * 4;

        bool changed = keyframe;
        if (!changed) {
          for (uint16_t row = 0; row < th; ++row) {
            const std::size_t at =
                offset + static_cast<std::size_t>(row) * stride;
            if (std::memcmp(pixels.data() + at, previous_.data() + at,
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
                      pixels.data() + at, static_cast<std::size_t>(tw) * 4);
          std::memcpy(previous_.data() + at, pixels.data() + at,
                      static_cast<std::size_t>(tw) * 4);
        }
        // CGContext leaves the alpha byte undefined with alphaNoneSkipFirst.
        for (std::size_t i = 0; i < blob.data.size(); i += 4) {
          blob.data[i + 3] = 255;
        }
        tiles.push_back(std::move(blob));
      }
    }

    std::vector<uint8_t> payload;
    std::string err;
    if (!encode_capture_frame(header, tiles, options_.compress, payload, err)) {
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
    publish(std::move(frame));
    have_previous_ = true;
  }

  capture_options options_;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
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
