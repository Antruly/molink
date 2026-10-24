#include "capture/capture.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
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

/// Win7 has no DXGI Desktop Duplication (it arrived with Windows 8), so this
/// backend grabs the composited desktop with BitBlt. It shares the tiling,
/// diffing and encoding path with the DXGI source; only the pixel acquisition
/// differs. Multi-monitor layouts are captured as one virtual screen.
class gdi_capture final : public capture_base {
 public:
  explicit gdi_capture(const capture_options& options)
      : capture_base(options.fps), options_(options) {}
  ~gdi_capture() override { stop(); }

  bool start(std::string& err) override {
    if (running_.load()) return true;
    screen_dc_ = GetDC(nullptr);
    if (screen_dc_ == nullptr) {
      err = "GetDC(NULL) failed";
      return false;
    }
    x_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    width_ = static_cast<uint16_t>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
    height_ = static_cast<uint16_t>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (width_ == 0 || height_ == 0) {
      err = "the desktop has no capture area";
      release();
      return false;
    }
    mem_dc_ = CreateCompatibleDC(screen_dc_);
    if (mem_dc_ == nullptr) {
      err = "CreateCompatibleDC failed";
      release();
      return false;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -static_cast<LONG>(height_);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(mem_dc_, &info, DIB_RGB_COLORS, &bits_,
                               nullptr, 0);
    if (bitmap_ == nullptr || bits_ == nullptr) {
      err = "CreateDIBSection failed";
      release();
      return false;
    }
    old_bitmap_ = SelectObject(mem_dc_, bitmap_);
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void stop() override {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    release();
  }

  std::string backend() const override { return "gdi"; }
  bool is_real() const override { return true; }

  std::vector<display_info> displays() const override {
    return {current_display_info()};
  }
  int current_display() const override { return 0; }
  display_info current_display_info() const override {
    display_info info;
    info.index = 0;
    info.name = "Desktop";
    info.x = x_;
    info.y = y_;
    info.width = width_;
    info.height = height_;
    info.input_width = width_;
    info.input_height = height_;
    info.primary = true;
    return info;
  }
  bool select_display(int index) override { return index == 0; }

  bool copy_pixels(std::vector<uint8_t>& out, int& width, int& height) override {
    std::lock_guard<std::mutex> lock(pixels_mutex_);
    if (bits_ == nullptr || width_ == 0 || height_ == 0) return false;
    const std::size_t bytes =
        static_cast<std::size_t>(width_) * height_ * 4;
    out.assign(static_cast<const uint8_t*>(bits_),
               static_cast<const uint8_t*>(bits_) + bytes);
    width = width_;
    height = height_;
    return true;
  }

 private:
  void release() {
    if (mem_dc_ != nullptr && old_bitmap_ != nullptr) {
      SelectObject(mem_dc_, old_bitmap_);
      old_bitmap_ = nullptr;
    }
    if (bitmap_ != nullptr) {
      DeleteObject(bitmap_);
      bitmap_ = nullptr;
    }
    bits_ = nullptr;
    if (mem_dc_ != nullptr) {
      DeleteDC(mem_dc_);
      mem_dc_ = nullptr;
    }
    if (screen_dc_ != nullptr) {
      ReleaseDC(nullptr, screen_dc_);
      screen_dc_ = nullptr;
    }
  }

  void run() {
    auto next = std::chrono::steady_clock::now();
    while (running_.load()) {
      if (!active_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        next = std::chrono::steady_clock::now();
        continue;
      }
      if (BitBlt(mem_dc_, 0, 0, width_, height_, screen_dc_, x_, y_,
                 SRCCOPY | CAPTUREBLT)) {
        note_grab(true, width_, height_);
        publish_frame();
        update_cursor();
      }
      next += std::chrono::milliseconds(1000 / std::max(1, target_fps()));
      const auto now = std::chrono::steady_clock::now();
      if (next < now) next = now;
      std::this_thread::sleep_until(next);
    }
  }

  void publish_frame() {
    const uint16_t tile =
        options_.tile_size == 0 ? 256 : options_.tile_size;
    const std::size_t stride = static_cast<std::size_t>(width_) * 4;
    const uint8_t* pixels = static_cast<const uint8_t*>(bits_);
    if (pixels == nullptr) return;

    std::lock_guard<std::mutex> lock(pixels_mutex_);
    const bool keyframe = keyframe_requested_.exchange(false) ||
                          !have_previous_ ||
                          previous_.size() != stride * height_;
    if (previous_.size() != stride * height_) {
      previous_.assign(stride * height_, 0);
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
    build_changed_tiles(pixels, stride, width_, height_, tile,
                        previous_.data(), keyframe, nullptr,
                        /*force_opaque=*/true,
                        quality_change_threshold(options_.change_threshold),
                        tiles);

    encode_options encoding;
    encoding.compress = options_.compress && quality_compress();
    encoding.format = quality_format();
    encoding.effort = quality_effort();

    std::vector<uint8_t> payload;
    std::string err;
    if (!encode_capture_frame(header, tiles, encoding, payload, err)) return;

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

  void update_cursor() {
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info)) return;

    cursor_snapshot snapshot;
    snapshot.valid = true;
    snapshot.x = info.ptScreenPos.x;
    snapshot.y = info.ptScreenPos.y;
    snapshot.serial = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(info.hCursor) & 0xffffffffu);
    if ((info.flags & CURSOR_SHOWING) != 0 && info.hCursor != nullptr) {
      draw_cursor(info.hCursor, snapshot);
    }
    set_cursor(std::move(snapshot));
  }

  static void draw_cursor(HCURSOR cursor, cursor_snapshot& snapshot) {
    ICONINFO icon{};
    if (!GetIconInfo(cursor, &icon)) return;

    const int width = GetSystemMetrics(SM_CXCURSOR);
    const int height = GetSystemMetrics(SM_CYCURSOR);
    if (width <= 0 || height <= 0) return;

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(nullptr);
    void* image_bits = nullptr;
    void* mask_bits = nullptr;
    HBITMAP image = CreateDIBSection(dc, &bitmap_info, DIB_RGB_COLORS,
                                     &image_bits, nullptr, 0);
    HBITMAP mask = CreateDIBSection(dc, &bitmap_info, DIB_RGB_COLORS, &mask_bits,
                                    nullptr, 0);
    if (dc == nullptr || image == nullptr || mask == nullptr ||
        image_bits == nullptr || mask_bits == nullptr) {
      if (image != nullptr) DeleteObject(image);
      if (mask != nullptr) DeleteObject(mask);
      if (dc != nullptr) DeleteDC(dc);
      return;
    }

    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    std::memset(image_bits, 0, bytes);
    std::memset(mask_bits, 0, bytes);
    HGDIOBJ previous = SelectObject(dc, image);
    DrawIconEx(dc, 0, 0, cursor, width, height, 0, nullptr, DI_IMAGE);
    SelectObject(dc, mask);
    DrawIconEx(dc, 0, 0, cursor, width, height, 0, nullptr, DI_MASK);
    SelectObject(dc, previous);

    const uint8_t* image_pixels = static_cast<const uint8_t*>(image_bits);
    const uint8_t* mask_pixels = static_cast<const uint8_t*>(mask_bits);
    snapshot.rgba.resize(bytes);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
      // DI_MASK paints white where the icon is transparent.
      const bool transparent =
          (mask_pixels[i * 4] | mask_pixels[i * 4 + 1] |
           mask_pixels[i * 4 + 2]) != 0;
      snapshot.rgba[i * 4 + 0] = image_pixels[i * 4 + 2];
      snapshot.rgba[i * 4 + 1] = image_pixels[i * 4 + 1];
      snapshot.rgba[i * 4 + 2] = image_pixels[i * 4 + 0];
      snapshot.rgba[i * 4 + 3] = transparent ? 0 : 255;
    }
    snapshot.width = static_cast<uint16_t>(width);
    snapshot.height = static_cast<uint16_t>(height);
    snapshot.hotspot_x = static_cast<uint16_t>(icon.xHotspot);
    snapshot.hotspot_y = static_cast<uint16_t>(icon.yHotspot);

    DeleteObject(image);
    DeleteObject(mask);
    DeleteDC(dc);
    if (icon.hbmColor != nullptr) DeleteObject(icon.hbmColor);
    if (icon.hbmMask != nullptr) DeleteObject(icon.hbmMask);
  }

  capture_options options_;
  HDC screen_dc_ = nullptr;
  HDC mem_dc_ = nullptr;
  HBITMAP bitmap_ = nullptr;
  HGDIOBJ old_bitmap_ = nullptr;
  void* bits_ = nullptr;
  int x_ = 0;
  int y_ = 0;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  std::vector<uint8_t> previous_;
  mutable std::mutex pixels_mutex_;
  bool have_previous_ = false;
  uint32_t seq_ = 0;
  std::thread thread_;
};

}  // namespace

std::unique_ptr<capture_source> make_windows_gdi_capture(
    const capture_options& options) {
  return std::unique_ptr<capture_source>(new gdi_capture(options));
}

}  // namespace molink

#else

namespace molink {
std::unique_ptr<capture_source> make_windows_gdi_capture(
    const capture_options&) {
  return nullptr;
}
}  // namespace molink

#endif
