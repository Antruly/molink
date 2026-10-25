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
      : capture_base(options.fps), options_(options) {}
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

  std::vector<display_info> displays() const override {
    std::lock_guard<std::mutex> lock(displays_mutex_);
    return displays_;
  }
  int current_display() const override { return output_index_.load(); }
  display_info current_display_info() const override {
    std::lock_guard<std::mutex> lock(displays_mutex_);
    const int index = output_index_.load();
    if (index >= 0 && index < static_cast<int>(displays_.size())) {
      return displays_[index];
    }
    return display_info{};
  }
  bool select_display(int index) override {
    std::lock_guard<std::mutex> lock(displays_mutex_);
    if (index < 0 || index >= static_cast<int>(displays_.size())) return false;
    requested_output_.store(index);
    return true;
  }

  bool copy_pixels(std::vector<uint8_t>& out, int& width,
                   int& height) override {
    std::lock_guard<std::mutex> lock(pixels_mutex_);
    if (previous_.empty() || width_ == 0 || height_ == 0) return false;
    out = previous_;
    width = width_;
    height = height_;
    return true;
  }

  bool supports_display_mode() const override { return true; }

  bool set_display_mode(uint16_t width, uint16_t height) override {
    const display_info info = current_display_info();
    if (info.name.empty() || width == 0 || height == 0) return false;
    const std::wstring device(info.name.begin(), info.name.end());
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(device.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
      return false;
    }
    mode.dmPelsWidth = width;
    mode.dmPelsHeight = height;
    mode.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
    const LONG result = ChangeDisplaySettingsExW(
        device.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY, nullptr);
    return result == DISP_CHANGE_SUCCESSFUL;
  }

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

      const int requested_output = requested_output_.exchange(-1);
      if (requested_output >= 0 &&
          requested_output != output_index_.load()) {
        std::string switch_error;
        if (switch_output(requested_output, switch_error) &&
            create_staging(switch_error)) {
          output_index_.store(requested_output);
          keyframe_requested_.store(true);
          last_full = std::chrono::steady_clock::now();
        } else {
          std::fprintf(stderr, "molink capture: display switch: %s\n",
                       switch_error.c_str());
          std::string recover_error;
          if (!switch_output(output_index_.load(), recover_error) ||
              !create_staging(recover_error)) {
            std::fprintf(stderr, "molink capture: display recovery: %s\n",
                         recover_error.c_str());
          }
        }
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

      const int fps = target_fps();
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
    safe_release(staging_);
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
    if (!enumerate_outputs(err)) return false;

    int wanted = requested_output_.exchange(-1);
    if (wanted < 0 || wanted >= static_cast<int>(display_dxgi_indices_.size())) {
      wanted = options_.display_index;
    }
    if (wanted < 0 ||
        wanted >= static_cast<int>(display_dxgi_indices_.size())) {
      wanted = 0;
    }
    if (!switch_output(wanted, err)) {
      if (wanted == 0 || !switch_output(0, err)) return false;
      wanted = 0;
    }
    output_index_.store(wanted);
    have_previous_ = false;
    return true;
  }

  bool enumerate_outputs(std::string& err) {
    std::vector<display_info> displays;
    std::vector<UINT> indices;
    for (UINT i = 0;; ++i) {
      IDXGIOutput* output = nullptr;
      const HRESULT result = adapter_->EnumOutputs(i, &output);
      if (result == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(result) || output == nullptr) break;
      DXGI_OUTPUT_DESC desc{};
      if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
        const int width =
            desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        const int height =
            desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        if (width > 0 && height > 0 && width <= 65535 && height <= 65535) {
          display_info info;
          info.index = static_cast<int>(displays.size());
          info.x = desc.DesktopCoordinates.left;
          info.y = desc.DesktopCoordinates.top;
          info.width = static_cast<uint16_t>(width);
          info.height = static_cast<uint16_t>(height);
          info.input_width = width;
          info.input_height = height;
          info.primary = displays.empty();
          char name[128] = {};
          WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1, name,
                              sizeof(name), nullptr, nullptr);
          info.name = name;
          if (info.name.empty()) {
            info.name = "Display " + std::to_string(info.index);
          }
          displays.push_back(std::move(info));
          indices.push_back(i);
        }
      }
      output->Release();
    }
    if (displays.empty()) {
      err = "no attached display output";
      return false;
    }
    std::lock_guard<std::mutex> lock(displays_mutex_);
    displays_ = std::move(displays);
    display_dxgi_indices_ = std::move(indices);
    return true;
  }

  bool switch_output(int index, std::string& err) {
    safe_release(dup_);
    safe_release(output1_);
    safe_release(output_);
    safe_release(staging_);
    if (index < 0 ||
        index >= static_cast<int>(display_dxgi_indices_.size())) {
      err = "display index out of range";
      return false;
    }
    const UINT dxgi_index = display_dxgi_indices_[index];
    if (FAILED(adapter_->EnumOutputs(dxgi_index, &output_)) ||
        output_ == nullptr) {
      err = "EnumOutputs failed";
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
    const int width =
        desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    const int height =
        desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
      err = "unexpected output size";
      return false;
    }
    width_ = static_cast<uint16_t>(width);
    height_ = static_cast<uint16_t>(height);
    have_previous_ = false;
    previous_.clear();
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

    std::lock_guard<std::mutex> pixels_lock(pixels_mutex_);
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
                        /*force_opaque=*/false,
                        quality_change_threshold(options_.change_threshold),
                        tiles);

    encode_options encoding;
    encoding.compress = options_.compress && quality_compress();
    encoding.format = quality_format();
    encoding.effort = quality_effort();

    std::vector<uint8_t> payload;
    std::string err;
    if (!encode_capture_frame(header, tiles, encoding, payload, err)) {
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
  mutable std::mutex pixels_mutex_;
  bool have_previous_ = false;
  uint32_t seq_ = 0;
  std::thread thread_;

  std::vector<display_info> displays_;
  std::vector<UINT> display_dxgi_indices_;
  mutable std::mutex displays_mutex_;
  std::atomic<int> output_index_{0};
  std::atomic<int> requested_output_{-1};
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
