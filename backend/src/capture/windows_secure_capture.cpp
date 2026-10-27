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
#include <mutex>
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
  if (reply.capacity() < 36u * 1024u * 1024u) {
    reply.reserve(36u * 1024u * 1024u);
  }
  reply.resize(36u * 1024u * 1024u);
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
    secure_desktop_ = info.find("\"desktop\":\"Default\"") ==
                      std::string::npos;
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
  bool is_secure_desktop() const override { return secure_desktop_; }
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
  bool copy_pixels(std::vector<uint8_t>& out, int& width,
                   int& height) override {
    std::lock_guard<std::mutex> lock(pixels_mutex_);
    if (previous_.empty() || width_ == 0 || height_ == 0) return false;
    out = previous_;
    width = width_;
    height = height_;
    return true;
  }

 private:
  void run() {
    auto next = std::chrono::steady_clock::now();
    auto next_info = next;
    while (running_.load()) {
      if (!active_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        next = std::chrono::steady_clock::now();
        next_info = next;
        continue;
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_info) {
        next_info = now + std::chrono::seconds(2);
        std::vector<uint8_t> info;
        if (helper_command("info", info)) {
          const std::string text(reinterpret_cast<const char*>(info.data()),
                                 info.size());
          secure_desktop_ = text.find("\"desktop\":\"Default\"") ==
                            std::string::npos;
        }
      }
      std::string err;
      if (capture_frame(err)) {
        if (!last_error.empty()) {
          std::fprintf(stderr, "molink capture: secure capture recovered\n");
          last_error.clear();
        }
      } else if (!err.empty() && err != last_error) {
        // The helper may have stopped; without this line the session simply
        // froze with no clue in the core log.
        std::fprintf(stderr, "molink capture: %s\n", err.c_str());
        last_error = err;
      }
      // GDI + a large pipe copy is much heavier than DXGI. The lock screen
      // only needs a slow refresh, but a normal desktop should feel live.
      const int cap = secure_desktop_ ? 8 : 15;
      const int fps = std::min(cap, std::max(1, target_fps()));
      next += std::chrono::milliseconds(1000 / fps);
      const auto after = std::chrono::steady_clock::now();
      if (next < after) next = after;
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
    // build_changed_tiles() rewrites previous_, so a concurrent tile refresh
    // must not copy it mid-update.
    std::lock_guard<std::mutex> pixels_lock(pixels_mutex_);
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
    const auto now = std::chrono::steady_clock::now();
    const int interval_ms =
        periodic_keyframe_ms() > 0 ? periodic_keyframe_ms() : 120000;
    const bool refresh_due =
        now - last_full_ > std::chrono::milliseconds(interval_ms);
    const bool keyframe = !have_previous_ ||
                          keyframe_requested_.exchange(false) ||
                          refresh_due;
    if (keyframe) last_full_ = now;
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
    const auto build_start = std::chrono::steady_clock::now();
    std::vector<protocol::tile_blob> tiles;
    build_changed_tiles(pixels, stride, static_cast<uint16_t>(width),
                        static_cast<uint16_t>(height), tile, previous_.data(),
                        keyframe, nullptr, /*force_opaque=*/true,
                        quality_change_threshold(options_.change_threshold),
                        tiles);
    encode_options encoding;
    encoding.compress = options_.compress && quality_compress();
    encoding.format = quality_format();
    encoding.effort = quality_effort();
    encoding.av1 = av1_for_frame();
    encoding.av1_qp = quality_av1_qp();
    encoding.av1_grayscale = quality_av1_grayscale();
    encoding.av1_downscale = quality_av1_downscale();
    encoding.av1_max_area_percent = quality_av1_max_area_percent();
    encoding.av1_full_frame = quality_av1_full_frame();
    std::vector<uint8_t> av1_payload;
    bool av1_only = false;
    std::vector<uint8_t> payload;
    if (!encode_capture_frame(frame_header, tiles, encoding, pixels, stride, payload,
                              &av1_payload, err, &av1_only)) {
      return false;
    }
    note_build_time(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - build_start)
            .count()));
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
    frame->av1_payload = std::move(av1_payload);
    frame->av1_only = av1_only;
    publish(std::move(frame));
    have_previous_ = true;
    return true;
  }

  capture_options options_;
  bool secure_desktop_ = true;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  std::vector<uint8_t> previous_;
  mutable std::mutex pixels_mutex_;
  bool have_previous_ = false;
  std::chrono::steady_clock::time_point last_full_{};
  uint32_t seq_ = 0;
  /// Last distinct helper/capture error, so the log shows it once.
  std::string last_error;
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
