// Device avatars: the host sends its avatar as a data URL in `welcome`
// (`preset:*` for the built-in colours, or a WebP picture uploaded in the
// web UI). Windows' WIC cannot decode WebP without the store extension, so a
// picture is decoded once with libwebp and cached as a plain 32-bit BMP under
// %LOCALAPPDATA%\moLink\avatars, then loaded through WIC like any image.
//
// Build with -DMOLINK_HAVE_WEBP and link webp (static) to enable the import
// path; without it the shell falls back to the initial-letter avatar.
#pragma once

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(MOLINK_HAVE_WEBP)
#include <webp/decode.h>
#endif

namespace molink::ui {

inline std::vector<uint8_t> avatar_base64_decode(const std::string& in) {
  static const signed char kTable[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
      52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
      -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
      15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
      -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
      41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
  };
  std::vector<uint8_t> out;
  out.reserve(in.size() * 3 / 4);
  int accumulator = 0;
  int bits = 0;
  for (unsigned char c : in) {
    if (c >= 128) continue;
    const int value = kTable[c];
    if (value < 0) continue;
    accumulator = (accumulator << 6) | value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
    }
  }
  return out;
}

inline uint64_t avatar_hash(const std::string& data) {
  uint64_t hash = 1469598103934665603ull;
  for (unsigned char c : data) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

/// 32-bit BGRA bottom-up BMP: trivially loadable through WIC everywhere.
inline bool avatar_write_bmp(const std::wstring& path, const uint8_t* bgra,
                             int width, int height) {
  if (width <= 0 || height <= 0) return false;
  const uint32_t stride = static_cast<uint32_t>(width) * 4;
  const uint32_t image_bytes = stride * static_cast<uint32_t>(height);
  const uint32_t file_bytes = 54 + image_bytes;
  std::vector<uint8_t> header(54, 0);
  header[0] = 'B';
  header[1] = 'M';
  auto put32 = [&header](size_t at, uint32_t value) {
    header[at] = static_cast<uint8_t>(value & 0xff);
    header[at + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    header[at + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
    header[at + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
  };
  put32(2, file_bytes);
  put32(10, 54);
  put32(14, 40);
  put32(18, static_cast<uint32_t>(width));
  put32(22, static_cast<uint32_t>(height));
  header[26] = 1;  // planes
  header[28] = 32; // bits per pixel
  put32(34, image_bytes);
  FILE* file = _wfopen(path.c_str(), L"wb");
  if (file == nullptr) return false;
  bool ok = std::fwrite(header.data(), 1, header.size(), file) ==
            header.size();
  for (int row = height - 1; row >= 0 && ok; --row) {
    ok = std::fwrite(bgra + static_cast<size_t>(row) * stride, 1, stride,
                     file) == stride;
  }
  std::fclose(file);
  return ok;
}

inline std::wstring avatar_cache_dir() {
  wchar_t base[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) {
    return std::wstring();
  }
  std::wstring dir = std::wstring(base) + L"\\moLink\\avatars";
  CreateDirectoryW((std::wstring(base) + L"\\moLink").c_str(), nullptr);
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

/// Decodes a `data:image/webp;base64,...` URL into the BMP cache and returns
/// the cached file path (empty when WebP support is not compiled in or the
/// picture cannot be decoded).
inline std::wstring avatar_import_data_url(const std::string& url) {
#if !defined(MOLINK_HAVE_WEBP)
  (void)url;
  return std::wstring();
#else
  const size_t comma = url.find(',');
  if (comma == std::string::npos) return std::wstring();
  const std::string payload = url.substr(comma + 1);
  const std::vector<uint8_t> bytes = avatar_base64_decode(payload);
  if (bytes.empty()) return std::wstring();
  int width = 0;
  int height = 0;
  if (!WebPGetInfo(bytes.data(), bytes.size(), &width, &height) ||
      width <= 0 || height <= 0 || width > 1024 || height > 1024) {
    return std::wstring();
  }
  std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
  if (WebPDecodeRGBAInto(bytes.data(), bytes.size(), rgba.data(), rgba.size(),
                         width * 4) == nullptr) {
    return std::wstring();
  }
  // RGBA -> BGRA
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    std::swap(rgba[i], rgba[i + 2]);
  }
  const std::wstring dir = avatar_cache_dir();
  if (dir.empty()) return std::wstring();
  wchar_t name[48] = {};
  std::swprintf(name, 48, L"\\avatar-%016llx.bmp",
                static_cast<unsigned long long>(avatar_hash(payload)));
  const std::wstring path = dir + name;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return path;
  }
  if (!avatar_write_bmp(path, rgba.data(), width, height)) {
    return std::wstring();
  }
  return path;
#endif
}

/// Cached WIC bitmap for a cached avatar BMP (device dependent, so the cache
/// is keyed by the render target).
inline ID2D1Bitmap* avatar_bitmap(ID2D1RenderTarget* target,
                                  const std::wstring& path) {
  static ID2D1RenderTarget* cached_target = nullptr;
  static std::unordered_map<std::wstring, ID2D1Bitmap*> cache;
  if (target != cached_target) {
    for (auto& item : cache) item.second->Release();
    cache.clear();
    cached_target = target;
  }
  const auto found = cache.find(path);
  if (found != cache.end()) return found->second;
  IWICImagingFactory* wic = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
      wic == nullptr) {
    return nullptr;
  }
  IWICBitmapDecoder* decoder = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* converter = nullptr;
  ID2D1Bitmap* bitmap = nullptr;
  if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr,
                                               GENERIC_READ,
                                               WICDecodeMetadataCacheOnLoad,
                                               &decoder)) &&
      decoder != nullptr && SUCCEEDED(decoder->GetFrame(0, &frame)) &&
      frame != nullptr && SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
      converter != nullptr &&
      SUCCEEDED(converter->Initialize(
          frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
          nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    target->CreateBitmapFromWicBitmap(converter, nullptr, &bitmap);
  }
  if (converter != nullptr) converter->Release();
  if (frame != nullptr) frame->Release();
  if (decoder != nullptr) decoder->Release();
  wic->Release();
  cache[path] = bitmap;
  return bitmap;
}

}  // namespace molink::ui
