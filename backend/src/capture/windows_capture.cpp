#include "capture/capture.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

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

template <typename T>
void safe_release(T*& pointer) {
  if (pointer != nullptr) {
    pointer->Release();
    pointer = nullptr;
  }
}

bool is_bgra(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_B8G8R8A8_UNORM ||
         format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

class windows_capture final : public capture_base {
 public:
  explicit windows_capture(const capture_options& options)
      : options_(options) {}
  ~windows_capture() override { stop(); }

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

  std::string backend() const override { return "dxgi-duplication"; }
  bool is_real() const override { return true; }

 private:
  void thread_main(std::promise<std::string>& ready) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string err;
    if (!init_device(err) || !create_duplication(err) || !create_staging(err)) {
      teardown();
      running_.store(false);
      ready.set_value(err.empty() ? "could not initialise DXGI duplication" : err);
      CoUninitialize();
      return;
    }
    ready.set_value(std::string());

    keyframe_requested_.store(true);
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
      const bool got = grab(pixels, grab_error);
      update_cursor();
      if (got) {
        const bool key = keyframe_requested_.exchange(false) || !have_previous_;
        const bool refresh_due =
            std::chrono::steady_clock::now() - last_full > std::chrono::seconds(30);
        publish_frame(pixels, key || refresh_due);
        if (key || refresh_due) last_full = std::chrono::steady_clock::now();
      } else if (grab_error == "access_lost") {
        safe_release(dup_);
        safe_release(staging_);
        have_previous_ = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::string retry_error;
        if (!create_duplication(retry_error) || !create_staging(retry_error)) {
          std::fprintf(stderr, "molink capture: %s\n", retry_error.c_str());
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
      }

      const int fps = options_.fps > 0 ? options_.fps : 15;
      next_allowed += std::chrono::milliseconds(1000 / fps);
      const auto now = std::chrono::steady_clock::now();
      if (next_allowed > now) std::this_thread::sleep_until(next_allowed);
      else next_allowed = now;
    }

    teardown();
    CoUninitialize();
  }

  bool init_device(std::string& err) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, &device_, &level, &context_);
    if (FAILED(result)) {
      result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device_, &level, &context_);
    }
    if (FAILED(result) || device_ == nullptr || context_ == nullptr) {
      err = "D3D11CreateDevice failed";
      return false;
    }
    return true;
  }

  bool create_duplication(std::string& err) {
    safe_release(dup_);
    safe_release(output1_);
    safe_release(output_);
    safe_release(adapter_);
    safe_release(dxgi_device_);

    if (FAILED(device_->QueryInterface(IID_IDXGIDevice,
                                       reinterpret_cast<void**>(&dxgi_device_)))) {
      err = "QueryInterface(IDXGIDevice) failed";
      return false;
    }
    if (FAILED(dxgi_device_->GetAdapter(&adapter_)) || adapter_ == nullptr) {
      err = "GetAdapter failed";
      return false;
    }
    if (FAILED(adapter_->EnumOutputs(0, &output_)) || output_ == nullptr) {
      err = "no display output (EnumOutputs)";
      return false;
    }
    if (FAILED(output_->QueryInterface(IID_IDXGIOutput1,
                                       reinterpret_cast<void**>(&output1_)))) {
      err = "the output does not support desktop duplication";
      return false;
    }
    if (FAILED(output1_->DuplicateOutput(device_, &dup_)) || dup_ == nullptr) {
      err = "DuplicateOutput failed (another screen capture may be active)";
      return false;
    }

    DXGI_OUTPUT_DESC desc{};
    output_->GetDesc(&desc);
    const int width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    const int height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
      err = "unexpected output size";
      return false;
    }
    width_ = static_cast<uint16_t>(width);
    height_ = static_cast<uint16_t>(height);
    have_previous_ = false;
    return true;
  }

  bool create_staging(std::string& err) {
    safe_release(staging_);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_)) ||
        staging_ == nullptr) {
      err = "CreateTexture2D(staging) failed";
      return false;
    }
    previous_.clear();
    have_previous_ = false;
    return true;
  }

  bool grab(std::vector<uint8_t>& pixels, std::string& err) {
    if (dup_ == nullptr || staging_ == nullptr) {
      err = "access_lost";
      return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource* resource = nullptr;
    const HRESULT result = dup_->AcquireNextFrame(100, &info, &resource);
    if (result == DXGI_ERROR_WAIT_TIMEOUT) {
      err.clear();
      return false;
    }
    if (result == DXGI_ERROR_ACCESS_LOST) {
      err = "access_lost";
      return false;
    }
    if (FAILED(result) || resource == nullptr) {
      err = "AcquireNextFrame failed";
      return false;
    }

    ID3D11Texture2D* texture = nullptr;
    const HRESULT queried = resource->QueryInterface(
        IID_ID3D11Texture2D, reinterpret_cast<void**>(&texture));
    resource->Release();
    if (FAILED(queried) || texture == nullptr) {
      dup_->ReleaseFrame();
      err = "frame is not a D3D11 texture";
      return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!is_bgra(desc.Format)) {
      texture->Release();
      dup_->ReleaseFrame();
      err = "unsupported desktop format (HDR or non-BGRA)";
      return false;
    }
    if (desc.Width != width_ || desc.Height != height_) {
      width_ = static_cast<uint16_t>(desc.Width);
      height_ = static_cast<uint16_t>(desc.Height);
      if (!create_staging(err)) {
        texture->Release();
        dup_->ReleaseFrame();
        return false;
      }
    }

    context_->CopyResource(staging_, texture);
    texture->Release();
    dup_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped))) {
      err = "Map(staging) failed";
      return false;
    }

    const std::size_t width = width_;
    const std::size_t height = height_;
    pixels.resize(width * height * 4);
    const uint8_t* source = static_cast<const uint8_t*>(mapped.pData);
    for (std::size_t y = 0; y < height; ++y) {
      const uint8_t* row = source + y * mapped.RowPitch;
      uint8_t* out = pixels.data() + y * width * 4;
      for (std::size_t x = 0; x < width; ++x) {
        out[x * 4 + 0] = row[x * 4 + 0];
        out[x * 4 + 1] = row[x * 4 + 1];
        out[x * 4 + 2] = row[x * 4 + 2];
        out[x * 4 + 3] = 255;
      }
    }
    context_->Unmap(staging_, 0);
    return true;
  }

  void publish_frame(std::vector<uint8_t>& pixels, bool keyframe) {
    const uint16_t tile =
        options_.tile_size == 0 ? 256 : options_.tile_size;
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
          (mask_pixels[i * 4] | mask_pixels[i * 4 + 1] | mask_pixels[i * 4 + 2]) != 0;
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

  void teardown() {
    safe_release(staging_);
    safe_release(dup_);
    safe_release(output1_);
    safe_release(output_);
    safe_release(adapter_);
    safe_release(dxgi_device_);
    safe_release(context_);
    safe_release(device_);
  }

  capture_options options_;
  ID3D11Device* device_ = nullptr;
  ID3D11DeviceContext* context_ = nullptr;
  IDXGIDevice* dxgi_device_ = nullptr;
  IDXGIAdapter* adapter_ = nullptr;
  IDXGIOutput* output_ = nullptr;
  IDXGIOutput1* output1_ = nullptr;
  IDXGIOutputDuplication* dup_ = nullptr;
  ID3D11Texture2D* staging_ = nullptr;

  uint16_t width_ = 0;
  uint16_t height_ = 0;
  std::vector<uint8_t> previous_;
  bool have_previous_ = false;
  uint32_t seq_ = 0;
  std::thread thread_;
};

}  // namespace

std::unique_ptr<capture_source> make_windows_capture(
    const capture_options& options) {
  return std::unique_ptr<capture_source>(new windows_capture(options));
}

}  // namespace molink

#else  // !_WIN32

namespace molink {
std::unique_ptr<capture_source> make_windows_capture(const capture_options&) {
  return nullptr;
}
}  // namespace molink

#endif
