#include "surface/frame_surface.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace molink {
namespace {

constexpr uint32_t kMagic = 0x5255534d;  // "MSUR"
constexpr uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 64;

struct surface_header {
  uint32_t magic;
  uint32_t version;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t format;
  uint64_t sequence;
  uint64_t timestamp_us;
  uint32_t slot_bytes;
  uint32_t reserved[5];
};
static_assert(sizeof(surface_header) <= kHeaderBytes,
              "the header must fit its reserved space");

uint8_t* slots(uint8_t* base) { return base + kHeaderBytes; }

}  // namespace

frame_surface& frame_surface::instance() {
  static frame_surface surface;
  return surface;
}

frame_surface::~frame_surface() { unmap(); }

bool frame_surface::active() const { return mapping_ != nullptr; }

std::string frame_surface::path() const { return path_; }

uint64_t frame_surface::sequence() const { return sequence_; }

bool frame_surface::map(int max_width, int max_height) {
  if (mapping_ != nullptr && max_width <= max_width_ &&
      max_height <= max_height_) {
    return true;
  }
  unmap();
  if (max_width <= 0 || max_height <= 0) return false;
  max_width_ = max_width;
  max_height_ = max_height;
  slot_bytes_ = static_cast<std::size_t>(max_width) * max_height * 4;
  bytes_ = kHeaderBytes + slot_bytes_ * 2;

#if defined(_WIN32)
  std::error_code ec;
  const std::filesystem::path file =
      std::filesystem::temp_directory_path(ec) /
      ("molink-surface-" + std::to_string(GetCurrentProcessId()) + ".bin");
  if (ec) return false;
  path_ = file.string();
  // FILE_SHARE_READ lets the shell's native window open the file while the
  // core keeps publishing into it.
  HANDLE handle = CreateFileW(file.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size;
  size.QuadPart = static_cast<LONGLONG>(bytes_);
  if (!SetFilePointerEx(handle, size, nullptr, FILE_BEGIN) ||
      !SetEndOfFile(handle)) {
    CloseHandle(handle);
    return false;
  }
  HANDLE mapping = CreateFileMappingW(handle, nullptr, PAGE_READWRITE, 0, 0,
                                      nullptr);
  CloseHandle(handle);
  if (mapping == nullptr) return false;
  mapping_ = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes_);
  CloseHandle(mapping);
  if (mapping_ == nullptr) return false;
#else
  std::error_code ec;
  const std::filesystem::path file =
      std::filesystem::path("/dev/shm") /
      ("molink-surface-" + std::to_string(::getpid()) + ".bin");
  path_ = file.string();
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd_ < 0) return false;
  if (::ftruncate(fd_, static_cast<off_t>(bytes_)) != 0) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }
  mapping_ = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (mapping_ == MAP_FAILED) {
    mapping_ = nullptr;
    ::close(fd_);
    fd_ = -1;
    return false;
  }
#endif

  auto* header = static_cast<surface_header*>(mapping_);
  std::memset(mapping_, 0, bytes_);
  header->magic = kMagic;
  header->version = kVersion;
  header->slot_bytes = static_cast<uint32_t>(slot_bytes_);
  return true;
}

void frame_surface::unmap() {
  if (mapping_ == nullptr) return;
#if defined(_WIN32)
  UnmapViewOfFile(mapping_);
#else
  ::munmap(mapping_, bytes_);
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
#endif
  mapping_ = nullptr;
  bytes_ = 0;
  slot_bytes_ = 0;
  max_width_ = 0;
  max_height_ = 0;
  if (!path_.empty()) {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    path_.clear();
  }
}

std::string frame_surface::enable(int max_width, int max_height) {
  if (!map(max_width, max_height)) return std::string();
  return path_;
}

void frame_surface::publish(const uint8_t* pixels, int width, int height,
                            int stride, format pixel_format) {
  if (mapping_ == nullptr || pixels == nullptr) return;
  if (width <= 0 || height <= 0 || width > max_width_ || height > max_height_) {
    return;
  }
  if (stride < width * 4) return;
  const uint64_t next = sequence_ + 1;
  uint8_t* target =
      slots(static_cast<uint8_t*>(mapping_)) + (next & 1) * slot_bytes_;
  const std::size_t row = static_cast<std::size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    std::memcpy(target + static_cast<std::size_t>(y) * row,
                pixels + static_cast<std::size_t>(y) * stride, row);
  }
  auto* header = static_cast<surface_header*>(mapping_);
  header->width = static_cast<uint32_t>(width);
  header->height = static_cast<uint32_t>(height);
  header->stride = static_cast<uint32_t>(row);
  header->format = static_cast<uint32_t>(pixel_format);
  header->slot_bytes = static_cast<uint32_t>(slot_bytes_);
#if defined(_WIN32)
  MemoryBarrier();
  header->sequence = next;
#else
  __atomic_store_n(&header->sequence, next, __ATOMIC_RELEASE);
#endif
  sequence_ = next;
}

bool init_frame_surface_from_env() {
  const char* flag = std::getenv("MOLINK_SURFACE");
  if (flag == nullptr || *flag == '\0' || std::strcmp(flag, "0") == 0) {
    return false;
  }
  // 4K is the ceiling for the two slots; a smaller screen simply uses less.
  const std::string path = frame_surface::instance().enable(3840, 2160);
  if (!path.empty()) {
    std::fprintf(stderr, "molink: native surface at %s\n", path.c_str());
  }
  return !path.empty();
}

}  // namespace molink
