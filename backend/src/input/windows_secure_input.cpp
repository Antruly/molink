#include "input/windows_secure_input.h"

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

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\molink-secure";

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

int vk_from_code(const std::string& code) {
  if (code.size() == 4 && code.compare(0, 3, "Key") == 0) {
    return std::toupper(static_cast<unsigned char>(code[3]));
  }
  if (code.size() == 6 && code.compare(0, 5, "Digit") == 0) {
    return code[5];
  }
  if (code.size() >= 2 && code[0] == 'F') {
    const int number = std::atoi(code.c_str() + 1);
    if (number >= 1 && number <= 12) return VK_F1 + number - 1;
  }
  static const struct {
    const char* code;
    int vk;
  } keys[] = {
      {"Space", VK_SPACE},          {"Enter", VK_RETURN},
      {"Tab", VK_TAB},              {"Escape", VK_ESCAPE},
      {"Backspace", VK_BACK},       {"Delete", VK_DELETE},
      {"Insert", VK_INSERT},        {"Home", VK_HOME},
      {"End", VK_END},              {"PageUp", VK_PRIOR},
      {"PageDown", VK_NEXT},        {"ArrowLeft", VK_LEFT},
      {"ArrowRight", VK_RIGHT},     {"ArrowUp", VK_UP},
      {"ArrowDown", VK_DOWN},       {"ShiftLeft", VK_LSHIFT},
      {"ShiftRight", VK_RSHIFT},    {"ControlLeft", VK_LCONTROL},
      {"ControlRight", VK_RCONTROL},{"AltLeft", VK_LMENU},
      {"AltRight", VK_RMENU},       {"MetaLeft", VK_LWIN},
      {"MetaRight", VK_RWIN},       {"CapsLock", VK_CAPITAL},
      {"Minus", VK_OEM_MINUS},      {"Equal", VK_OEM_PLUS},
      {"BracketLeft", VK_OEM_4},    {"BracketRight", VK_OEM_6},
      {"Backslash", VK_OEM_5},      {"Semicolon", VK_OEM_1},
      {"Quote", VK_OEM_7},          {"Backquote", VK_OEM_3},
      {"Comma", VK_OEM_COMMA},      {"Period", VK_OEM_PERIOD},
      {"Slash", VK_OEM_2},
  };
  for (const auto& key : keys) {
    if (code == key.code) return key.vk;
  }
  return 0;
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
    const int vk = vk_from_code(code);
    if (vk == 0) return status::rejected;
    return send_command("key " + std::to_string(vk) + " " +
                        (down ? "1" : "0"))
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
  std::string reply;
  if (!send_command("info", &reply)) return nullptr;
  return std::unique_ptr<input_injector>(new secure_input());
}

}  // namespace molink
#else
namespace molink {
std::unique_ptr<input_injector> make_windows_secure_input() { return nullptr; }
}  // namespace molink
#endif
