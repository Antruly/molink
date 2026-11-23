#include "input/windows_secure_input.h"
#include "input/keymap.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace molink {
namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\molink-secure-input";

bool send_command(const std::string& command, std::string* reply = nullptr) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    WaitNamedPipeW(kPipeName, 100);
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      Sleep(30);
      continue;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(pipe, command.data(), static_cast<DWORD>(command.size()),
              &written, nullptr);
    if (reply != nullptr) {
      char buffer[4096] = {};
      DWORD read = 0;
      ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, nullptr);
      reply->assign(buffer, read);
    }
    CloseHandle(pipe);
    return true;
  }
  return false;
}

/// Resolve through the shared table so the lock/UAC desktop accepts the same
/// keys as the normal backend. The pipe carries a VK plus the extended flag;
/// scan-code-only keys (Power/WakeUp) cannot be expressed there.
bool vk_from_code(const std::string& code, WORD& vk, bool& extended) {
  const key_lookup mapping = lookup_key(keymap_platform::windows, code);
  if (!mapping.found || mapping.unsupported || mapping.scan_code) return false;
  vk = static_cast<WORD>(mapping.value);
  extended = mapping.extended;
  return true;
}

class secure_input final : public input_injector {
 public:
  secure_input() {
    std::string reply;
    if (send_command("info", &reply)) {
      const std::size_t wpos = reply.find("\"width\":");
      const std::size_t hpos = reply.find("\"height\":");
      if (wpos != std::string::npos) {
        width_ = std::atoi(reply.c_str() + wpos + 8);
      }
      if (hpos != std::string::npos) {
        height_ = std::atoi(reply.c_str() + hpos + 9);
      }
    }
  }

  bool available() const override { return true; }
  std::string backend() const override { return "secure-desktop"; }

  // The secure desktop can have a different resolution than the normal
  // session (often 1024x768). The helper's info is authoritative; refresh it
  // while moving so lock-screen clicks map to the right pixels.
  void set_display_rect(int x, int y, int width, int height) override {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
  }

  status move(double nx, double ny) override {
    refresh_size();
    const int x = static_cast<int>(nx * std::max(1, width_));
    const int y = static_cast<int>(ny * std::max(1, height_));
    return send_command("move " + std::to_string(x) + " " +
                        std::to_string(y))
               ? status::ok
               : status::unavailable;
  }

  status button(int button, bool down) override {
    return send_command("button " + std::to_string(button) + " " +
                        (down ? "1" : "0"))
               ? status::ok
               : status::unavailable;
  }

  status wheel(int dx, int dy) override {
    return send_command("wheel " + std::to_string(dx) + " " +
                        std::to_string(dy))
               ? status::ok
               : status::unavailable;
  }

  status key(const std::string& code, bool down) override {
    WORD vk = 0;
    bool extended = false;
    if (!vk_from_code(code, vk, extended)) return status::rejected;
    // The fourth field is optional for an older helper, which ignores it.
    return send_command("key " + std::to_string(vk) + " " +
                        (down ? "1" : "0") + " " + (extended ? "1" : "0"))
               ? status::ok
               : status::unavailable;
  }

  status text(const std::string& utf8) override {
    return send_command("text " + utf8) ? status::ok : status::unavailable;
  }

  void release_all() override { send_command("release-all"); }

 private:
  void refresh_size() {
    const auto now = std::chrono::steady_clock::now();
    if (last_info_ != std::chrono::steady_clock::time_point{} &&
        now - last_info_ < std::chrono::milliseconds(500)) {
      return;
    }
    last_info_ = now;
    std::string reply;
    if (!send_command("info", &reply)) return;
    const std::size_t wpos = reply.find("\"width\":");
    const std::size_t hpos = reply.find("\"height\":");
    if (wpos != std::string::npos) width_ = std::atoi(reply.c_str() + wpos + 8);
    if (hpos != std::string::npos) height_ = std::atoi(reply.c_str() + hpos + 9);
  }

  int width_ = 1920;
  int height_ = 1080;
  std::chrono::steady_clock::time_point last_info_{};
};

}  // namespace

std::unique_ptr<input_injector> make_windows_secure_input() {
  // Do not fail just because the one-shot info probe lost the pipe race with
  // a frame transfer: input must still be attempted, and the size refreshes
  // on the first move.
  return std::unique_ptr<input_injector>(new secure_input());
}

}  // namespace molink
#else
namespace molink {
std::unique_ptr<input_injector> make_windows_secure_input() { return nullptr; }
}  // namespace molink
#endif
