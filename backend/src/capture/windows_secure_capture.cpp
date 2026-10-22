#include "capture/windows_secure_capture.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "capture/frame_encode.h"
#include "protocol/frame_packet.h"

namespace molink {
namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\molink-secure";

HANDLE open_helper(DWORD wait_ms = 0) {
  if (wait_ms != 0) WaitNamedPipeW(kPipeName, wait_ms);
  HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return pipe;
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
  return pipe;
}

bool helper_command(const std::string& command, std::vector<uint8_t>& reply) {
  HANDLE pipe = open_helper(200);
  if (pipe == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  WriteFile(pipe, command.data(), static_cast<DWORD>(command.size()), &written,
            nullptr);
  reply.resize(64u * 1024u * 1024u);
  DWORD read = 0;
  const bool ok = ReadFile(pipe, reply.data(),
                           static_cast<DWORD>(reply.size()), &read, nullptr) != 0;
  CloseHandle(pipe);
  reply.resize(ok ? read : 0);
  return ok;
}

class secure_capture final : public capture_base {
 public:
  explicit secure_capture(const capture_options& options)
      : capture_base(options.fps), options_(options) {}

  ~secure_capture() override { stop(); }

  bool start(std::string& err) override {
    // The service may still be starting the helper; retry briefly.
    std::vector<uint8_t> reply;
    bool ok = false;
    for (int attempt = 0; attempt < 30; ++attempt) {
      if (helper_command("info", reply) && reply.size() > 8) {
        ok = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ok) {
      err = "secure desktop helper is not available";
      return false;
    }
    const std::string info(reinterpret_cast<const char*>(reply.data()),
                           reply.size());
    const std::size_t wpos = info.find("\"width\":");
    const std::size_t hpos = info.find("\"height\":");
    if (wpos == std::string::npos || hpos == std::string::npos) {
      err = "secure desktop info is malformed";
      return false;
    }
    width_ = static_cast<uint16_t>(std::atoi(info.c_str() + wpos + 8));
    height_ = static_cast<uint16_t>(std::atoi(info.c_str() + hpos + 9));
    if (width_ == 0 || height_ == 0) {
      err = "secure desktop has no size";
      return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void stop() override {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
  }

  std::string backend() const override { return "secure-desktop"; }
  bool is_real() const override { return true; }
  std::vector<display_info> displays() const override {
    display_info info;
    info.name = "Secure desktop";
    info.width = width_;
    info.height = height_;
    info.input_width = width_;
    info.input_height = height_;
    info.primary = true;
    return {info};
  }
  int current_display() const override { return 0; }
  display_info current_display_info() const override {
    return displays().front();
  }
  bool select_display(int index) override { return index == 0; }
  bool cursor(cursor_snapshot&) const override { return false; }

 private:
  void run() {
    auto next = std::chrono::steady_clock::now();
    while (running_.load()) {
      if (!active_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        next = std::chrono::steady_clock::now();
        continue;
      }
      std::string err;
      capture_frame(err);
      next += std::chrono::milliseconds(1000 / std::max(1, target_fps()));
      const auto now = std::chrono::steady_clock::now();
      if (next < now) next = now;
      std::this_thread::sleep_until(next);
    }
  }

  bool capture_frame(std::string& err) {
    std::vector<uint8_t> reply;
    if (!helper_command("capture", reply)) {
      err = "secure capture failed";
      return false;
    }
    const char* header = reinterpret_cast<const char*>(reply.data());
    const char* newline = static_cast<const char*>(
        std::memchr(header, '\n', std::min<std::size_t>(reply.size(), 64)));
    int width = 0;
    int height = 0;
    std::size_t length = 0;
    if (newline == nullptr ||
        std::sscanf(header, "FRAME %d %d %zu", &width, &height, &length) != 3 ||
        length != static_cast<std::size_t>(width) * height * 4) {
      err = "secure frame header is malformed";
      return false;
    }
    const std::size_t offset =
        static_cast<std::size_t>(newline - header) + 1;
    if (offset + length > reply.size()) {
      err = "secure frame is truncated";
      return false;
    }
    const uint8_t* pixels = reply.data() + offset;
    if (width != width_ || height != height_) {
      width_ = static_cast<uint16_t>(width);
      height_ = static_cast<uint16_t>(height);
      previous_.clear();
      have_previous_ = false;
    }
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    if (previous_.size() != static_cast<std::size_t>(width) * height * 4) {
      previous_.assign(static_cast<std::size_t>(width) * height * 4, 0);
      have_previous_ = false;
    }
    const bool keyframe = !have_previous_ || keyframe_requested_.exchange(false);
    const uint16_t tile = std::max<uint16_t>(
        64, options_.tile_size == 0 ? 256 : options_.tile_size);
    protocol::frame_header frame_header;
    frame_header.frame_seq = seq_++;
    frame_header.codec = static_cast<uint8_t>(protocol::codec::bgra);
    frame_header.flags = keyframe ? protocol::kFrameFlagKeyframe
                                  : protocol::kFrameFlagPartial;
    frame_header.screen_w = static_cast<uint16_t>(width);
    frame_header.screen_h = static_cast<uint16_t>(height);
    frame_header.tile_w = tile;
    frame_header.tile_h = tile;
    std::vector<protocol::tile_blob> tiles;
    build_changed_tiles(pixels, stride, static_cast<uint16_t>(width),
                        static_cast<uint16_t>(height), tile, previous_.data(),
                        keyframe, nullptr, /*force_opaque=*/true,
                        quality_change_threshold(options_.change_threshold),
                        tiles);
    encode_options encoding;
    encoding.compress = options_.compress;
    encoding.format = quality_format();
    encoding.effort = quality_effort();
    std::vector<uint8_t> payload;
    if (!encode_capture_frame(frame_header, tiles, encoding, payload, err)) {
      return false;
    }
    auto frame = std::make_shared<captured_frame>();
    frame->seq = frame_header.frame_seq;
    frame->ts_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    frame->keyframe = keyframe;
    frame->width = static_cast<uint16_t>(width);
    frame->height = static_cast<uint16_t>(height);
    frame->tile_w = tile;
    frame->tile_h = tile;
    frame->codec = frame_header.codec;
    frame->payload = std::move(payload);
    publish(std::move(frame));
    have_previous_ = true;
    return true;
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

std::unique_ptr<capture_source> make_windows_secure_capture(
    const capture_options& options) {
  return std::unique_ptr<capture_source>(new secure_capture(options));
}

}  // namespace molink
#else
namespace molink {
std::unique_ptr<capture_source> make_windows_secure_capture(
    const capture_options&) {
  return nullptr;
}
}  // namespace molink
#endif
