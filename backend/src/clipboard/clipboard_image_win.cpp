#include "clipboard/clipboard_image_win.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace molink {
namespace {

/// Do not let a crafted image make the core allocate an unbounded buffer.
constexpr uint64_t kMaxDecodedPixels = 64ull * 1024 * 1024;  // 64 Mpixel
constexpr uint32_t kMaxDimension = 32768;

class com_scope {
 public:
  com_scope() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // S_FALSE means this thread already initialized COM and still owes one
    // CoUninitialize. RPC_E_CHANGED_MODE means another apartment mode owns the
    // thread; COM is usable and must not be uninitialized here.
    uninit_ = hr == S_OK || hr == S_FALSE;
  }
  ~com_scope() {
    if (uninit_) CoUninitialize();
  }
  com_scope(const com_scope&) = delete;
  com_scope& operator=(const com_scope&) = delete;

 private:
  bool uninit_ = false;
};

template <typename T>
class com_ptr {
 public:
  com_ptr() = default;
  ~com_ptr() { reset(); }
  com_ptr(const com_ptr&) = delete;
  com_ptr& operator=(const com_ptr&) = delete;

  T** put() {
    reset();
    return &value_;
  }
  T* get() const { return value_; }
  T* operator->() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }

  void reset() {
    if (value_ != nullptr) {
      value_->Release();
      value_ = nullptr;
    }
  }

 private:
  T* value_ = nullptr;
};

IWICImagingFactory* create_factory() {
  IWICImagingFactory* factory = nullptr;
  const HRESULT hr = CoCreateInstance(
      CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&factory));
  return SUCCEEDED(hr) ? factory : nullptr;
}

/// An IStream over a private copy of the bytes (CreateStreamOnHGlobal takes
/// ownership of the HGLOBAL).
IStream* stream_from_bytes(const std::vector<uint8_t>& bytes) {
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
  if (memory == nullptr) return nullptr;
  void* target = GlobalLock(memory);
  if (target == nullptr) {
    GlobalFree(memory);
    return nullptr;
  }
  if (!bytes.empty()) {
    std::memcpy(target, bytes.data(), bytes.size());
  }
  GlobalUnlock(memory);
  IStream* stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
    GlobalFree(memory);
    return nullptr;
  }
  return stream;
}

bool read_stream(IStream* stream, std::vector<uint8_t>& out) {
  if (stream == nullptr) return false;
  STATSTG stat = {};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return false;
  const ULONGLONG length = stat.cbSize.QuadPart;
  if (length == 0 || length > 512ull * 1024 * 1024) return false;
  HGLOBAL memory = nullptr;
  if (FAILED(GetHGlobalFromStream(stream, &memory)) || memory == nullptr) {
    return false;
  }
  const SIZE_T capacity = GlobalSize(memory);
  if (capacity < length) return false;
  void* data = GlobalLock(memory);
  if (data == nullptr) return false;
  out.assign(static_cast<const uint8_t*>(data),
             static_cast<const uint8_t*>(data) + length);
  GlobalUnlock(memory);
  return true;
}

/// Decode any PNG WIC understands into top-down 32-bit BGRA.
bool decode_png(const std::vector<uint8_t>& png, uint32_t& width,
                uint32_t& height, std::vector<uint8_t>& bgra) {
  if (png.empty()) return false;
  com_scope com;
  com_ptr<IWICImagingFactory> factory;
  factory.put()[0] = create_factory();
  if (!factory) return false;

  com_ptr<IStream> stream;
  stream.put()[0] = stream_from_bytes(png);
  if (!stream) return false;

  com_ptr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(
          stream.get(), nullptr, WICDecodeMetadataCacheOnDemand,
          decoder.put()))) {
    return false;
  }
  com_ptr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.put()))) return false;
  UINT w = 0;
  UINT h = 0;
  if (FAILED(frame->GetSize(&w, &h))) return false;
  if (w == 0 || h == 0 || w > kMaxDimension || h > kMaxDimension) return false;
  if (static_cast<uint64_t>(w) * h > kMaxDecodedPixels) return false;

  com_ptr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(converter.put()))) return false;
  if (FAILED(converter->Initialize(
          frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
          nullptr, 0, WICBitmapPaletteTypeCustom))) {
    return false;
  }
  width = w;
  height = h;
  const uint64_t stride = static_cast<uint64_t>(w) * 4;
  bgra.assign(static_cast<std::size_t>(stride * h), 0);
  if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                   static_cast<UINT>(bgra.size()),
                                   bgra.data()))) {
    return false;
  }
  return true;
}

bool encode_png(const std::vector<uint8_t>& bgra, uint32_t width,
                uint32_t height, std::vector<uint8_t>& png) {
  if (width == 0 || height == 0) return false;
  const uint64_t stride = static_cast<uint64_t>(width) * 4;
  if (bgra.size() < stride * height) return false;
  com_scope com;
  com_ptr<IWICImagingFactory> factory;
  factory.put()[0] = create_factory();
  if (!factory) return false;

  com_ptr<IWICBitmap> bitmap;
  if (FAILED(factory->CreateBitmapFromMemory(
          width, height, GUID_WICPixelFormat32bppBGRA,
          static_cast<UINT>(stride), static_cast<UINT>(bgra.size()),
          const_cast<BYTE*>(bgra.data()), bitmap.put()))) {
    return false;
  }

  com_ptr<IStream> memory;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, memory.put()))) {
    return false;
  }
  com_ptr<IWICStream> stream;
  if (FAILED(factory->CreateStream(stream.put()))) return false;
  if (FAILED(stream->InitializeFromIStream(memory.get()))) return false;

  com_ptr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                    encoder.put()))) {
    return false;
  }
  if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
    return false;
  }
  com_ptr<IWICBitmapFrameEncode> frame;
  com_ptr<IPropertyBag2> properties;
  if (FAILED(encoder->CreateNewFrame(frame.put(), properties.put()))) {
    return false;
  }
  if (FAILED(frame->Initialize(properties.get()))) return false;
  if (FAILED(frame->SetSize(width, height))) return false;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format))) return false;
  if (FAILED(frame->WriteSource(bitmap.get(), nullptr))) return false;
  if (FAILED(frame->Commit())) return false;
  if (FAILED(encoder->Commit())) return false;
  return read_stream(memory.get(), png);
}

uint32_t read_u32(const uint8_t* data) {
  uint32_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

int32_t read_i32(const uint8_t* data) {
  int32_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

unsigned channel_from_mask(uint32_t pixel, uint32_t mask) {
  if (mask == 0) return 0;
  unsigned shift = 0;
  while (((mask >> shift) & 1u) == 0 && shift < 32) ++shift;
  const uint32_t field = mask >> shift;
  unsigned bits = 0;
  while (((field >> bits) & 1u) != 0 && bits < 32) ++bits;
  if (bits == 0) return 0;
  const uint32_t value = (pixel & mask) >> shift;
  const uint32_t max_value = bits >= 32 ? 0xffffffffu : ((1u << bits) - 1u);
  return static_cast<unsigned>((value * 255u + max_value / 2u) / max_value);
}

bool dib_to_bgra(const std::vector<uint8_t>& dib, uint32_t& width,
                 uint32_t& height, std::vector<uint8_t>& bgra) {
  if (dib.size() < 40) return false;
  const uint32_t header_size = read_u32(dib.data());
  if (header_size < 40 || header_size > dib.size()) return false;
  const int32_t signed_width = read_i32(dib.data() + 4);
  const int32_t signed_height = read_i32(dib.data() + 8);
  const uint16_t planes = static_cast<uint16_t>(dib[12] | (dib[13] << 8));
  const uint16_t bit_count = static_cast<uint16_t>(dib[14] | (dib[15] << 8));
  const uint32_t compression = read_u32(dib.data() + 16);
  if (signed_width <= 0 || signed_height == 0 || planes != 1) return false;
  const uint32_t w = static_cast<uint32_t>(signed_width);
  const bool top_down = signed_height < 0;
  const uint32_t h = top_down
                         ? static_cast<uint32_t>(-static_cast<int64_t>(
                               signed_height))
                         : static_cast<uint32_t>(signed_height);
  if (w > kMaxDimension || h > kMaxDimension) return false;
  if (static_cast<uint64_t>(w) * h > kMaxDecodedPixels) return false;
  if (bit_count != 24 && bit_count != 32) return false;
  if (compression != BI_RGB && compression != BI_BITFIELDS) return false;
  if (compression == BI_BITFIELDS && header_size < 40) return false;

  const uint64_t source_stride =
      ((static_cast<uint64_t>(w) * bit_count + 31) / 32) * 4;
  if (source_stride > dib.size() ||
      source_stride * h > dib.size() - header_size) {
    return false;
  }
  const uint8_t* pixels = dib.data() + header_size;

  uint32_t red_mask = bit_count == 32 ? 0x00ff0000u : 0x00ff0000u;
  uint32_t green_mask = 0x0000ff00u;
  uint32_t blue_mask = 0x000000ffu;
  uint32_t alpha_mask = 0;
  if (compression == BI_BITFIELDS && header_size >= 52) {
    red_mask = read_u32(dib.data() + 40);
    green_mask = read_u32(dib.data() + 44);
    blue_mask = read_u32(dib.data() + 48);
    if (header_size >= 56) alpha_mask = read_u32(dib.data() + 52);
  }

  width = w;
  height = h;
  bgra.assign(static_cast<std::size_t>(w) * h * 4, 0);
  for (uint32_t y = 0; y < h; ++y) {
    const uint32_t source_y = top_down ? y : (h - 1 - y);
    const uint8_t* row = pixels + source_y * source_stride;
    uint8_t* target = bgra.data() + static_cast<std::size_t>(y) * w * 4;
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* out = target + x * 4;
      if (bit_count == 24) {
        out[0] = row[x * 3 + 0];
        out[1] = row[x * 3 + 1];
        out[2] = row[x * 3 + 2];
        out[3] = 255;
      } else {
        const uint32_t pixel = read_u32(row + x * 4);
        if (compression == BI_BITFIELDS) {
          out[0] = static_cast<uint8_t>(channel_from_mask(pixel, blue_mask));
          out[1] = static_cast<uint8_t>(channel_from_mask(pixel, green_mask));
          out[2] = static_cast<uint8_t>(channel_from_mask(pixel, red_mask));
          out[3] = alpha_mask != 0
                       ? static_cast<uint8_t>(
                             channel_from_mask(pixel, alpha_mask))
                       : 255;
        } else {
          out[0] = row[x * 4 + 0];
          out[1] = row[x * 4 + 1];
          out[2] = row[x * 4 + 2];
          // A BI_RGB 32-bit DIB's fourth byte is reserved, not alpha. Treating
          // it as alpha made screenshots pasted through CF_DIB fully
          // transparent in some viewers.
          out[3] = 255;
        }
      }
    }
  }
  return true;
}

}  // namespace

bool clipboard_png_to_dib(const std::vector<uint8_t>& png,
                          std::vector<uint8_t>& dib) {
  try {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;
    if (!decode_png(png, width, height, bgra)) return false;
    if (static_cast<uint64_t>(width) * height * 4 > 0xffffffffull) return false;

    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    dib.assign(40 + stride * height, 0);
    auto* header = reinterpret_cast<BITMAPINFOHEADER*>(dib.data());
    header->biSize = 40;
    header->biWidth = static_cast<LONG>(width);
    // A positive height means bottom-up, which is what CF_DIB consumers
    // expect.
    header->biHeight = static_cast<LONG>(height);
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(stride * height);
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(dib.data() + 40 + static_cast<std::size_t>(height - 1 - y) *
                                       stride,
                  bgra.data() + static_cast<std::size_t>(y) * stride, stride);
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool clipboard_dib_to_png(const std::vector<uint8_t>& dib,
                          std::vector<uint8_t>& png) {
  try {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;
    if (!dib_to_bgra(dib, width, height, bgra)) return false;
    png.clear();
    return encode_png(bgra, width, height, png);
  } catch (...) {
    return false;
  }
}

}  // namespace molink

#endif  // _WIN32
