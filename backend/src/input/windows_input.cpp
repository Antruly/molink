#include "input/input.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "util.h"

namespace molink {
namespace {

struct NamedKey {
  const char* code;
  WORD virtual_key;
  bool extended;
};

const NamedKey kNamedKeys[] = {
    {"Enter", VK_RETURN, false},       {"NumpadEnter", VK_RETURN, true},
    {"Backspace", VK_BACK, false},     {"Tab", VK_TAB, false},
    {"Space", VK_SPACE, false},        {"Escape", VK_ESCAPE, false},
    {"Delete", VK_DELETE, true},       {"Insert", VK_INSERT, true},
    {"Home", VK_HOME, true},           {"End", VK_END, true},
    {"PageUp", VK_PRIOR, true},        {"PageDown", VK_NEXT, true},
    {"ArrowLeft", VK_LEFT, true},      {"ArrowRight", VK_RIGHT, true},
    {"ArrowUp", VK_UP, true},          {"ArrowDown", VK_DOWN, true},
    {"ShiftLeft", VK_LSHIFT, false},   {"ShiftRight", VK_RSHIFT, false},
    {"ControlLeft", VK_LCONTROL, false}, {"ControlRight", VK_RCONTROL, false},
    {"AltLeft", VK_LMENU, false},      {"AltRight", VK_RMENU, true},
    {"MetaLeft", VK_LWIN, true},       {"MetaRight", VK_RWIN, true},
    {"CapsLock", VK_CAPITAL, false},   {"NumLock", VK_NUMLOCK, true},
    {"ScrollLock", VK_SCROLL, false},  {"PrintScreen", VK_SNAPSHOT, true},
    {"Pause", VK_PAUSE, false},        {"ContextMenu", VK_APPS, true},
    {"Minus", VK_OEM_MINUS, false},    {"Equal", VK_OEM_PLUS, false},
    {"BracketLeft", VK_OEM_4, false},  {"BracketRight", VK_OEM_6, false},
    {"Backslash", VK_OEM_5, false},    {"Semicolon", VK_OEM_1, false},
    {"Quote", VK_OEM_7, false},        {"Backquote", VK_OEM_3, false},
    {"Comma", VK_OEM_COMMA, false},    {"Period", VK_OEM_PERIOD, false},
    {"Slash", VK_OEM_2, false},
};

bool resolve_key(const std::string& code, WORD& virtual_key, bool& extended) {
  if (code.size() == 4 && code.rfind("Key", 0) == 0) {
    virtual_key = static_cast<WORD>(code[3]);  // 'A'..'Z'
    extended = false;
    return true;
  }
  if (code.size() == 6 && code.rfind("Digit", 0) == 0) {
    virtual_key = static_cast<WORD>(code[5]);  // '0'..'9'
    extended = false;
    return true;
  }
  if (code.size() >= 2 && code[0] == 'F') {
    const std::string digits = code.substr(1);
    const bool numeric = !digits.empty() &&
                         std::all_of(digits.begin(), digits.end(),
                                     [](char c) { return c >= '0' && c <= '9'; });
    if (numeric) {
      const int index = std::stoi(digits);
      if (index >= 1 && index <= 24) {
        virtual_key = static_cast<WORD>(VK_F1 + index - 1);
        extended = false;
        return true;
      }
    }
  }
  for (const NamedKey& named : kNamedKeys) {
    if (code == named.code) {
      virtual_key = named.virtual_key;
      extended = named.extended;
      return true;
    }
  }
  return false;
}

class windows_input final : public input_injector {
 public:
  ~windows_input() override { release_all(); }

  bool available() const override { return true; }
  std::string backend() const override { return "sendinput"; }

  void set_display_rect(int x, int y, int width, int height) override {
    if (width <= 0 || height <= 0) {
      has_display_ = false;
      return;
    }
    display_.left = x;
    display_.top = y;
    display_.right = x + width;
    display_.bottom = y + height;
    has_display_ = true;
  }

  status move(double nx, double ny) override {
    if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) return status::rejected;
    const int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtual_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtual_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtual_w <= 1 || virtual_h <= 1) return status::unavailable;

    int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    if (has_display_) {
      left = display_.left;
      top = display_.top;
      width = display_.right - display_.left;
      height = display_.bottom - display_.top;
    }
    if (width <= 0 || height <= 0) return status::unavailable;

    const int x = left + static_cast<int>(std::lround(nx * (width - 1)));
    const int y = top + static_cast<int>(std::lround(ny * (height - 1)));
    const long screen_x = std::clamp(
        static_cast<long>(std::lround((x - virtual_x) * 65535.0 /
                                      (virtual_w - 1))),
        0L, 65535L);
    const long screen_y = std::clamp(
        static_cast<long>(std::lround((y - virtual_y) * 65535.0 /
                                      (virtual_h - 1))),
        0L, 65535L);

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags =
        MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    input.mi.dx = static_cast<LONG>(screen_x);
    input.mi.dy = static_cast<LONG>(screen_y);
    return SendInput(1, &input, sizeof(input)) == 1 ? status::ok
                                                    : status::unavailable;
  }

  status button(int button_number, bool down) override {
    if (button_number < 1 || button_number > 5) return status::rejected;
    INPUT input{};
    input.type = INPUT_MOUSE;
    if (button_number <= 3) {
      static const DWORD down_flags[] = {0, MOUSEEVENTF_LEFTDOWN,
                                         MOUSEEVENTF_MIDDLEDOWN,
                                         MOUSEEVENTF_RIGHTDOWN};
      static const DWORD up_flags[] = {0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP,
                                       MOUSEEVENTF_RIGHTUP};
      input.mi.dwFlags = down ? down_flags[button_number] : up_flags[button_number];
    } else {
      input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
      input.mi.mouseData =
          button_number == 4 ? XBUTTON1 : XBUTTON2;
    }
    if (SendInput(1, &input, sizeof(input)) != 1) return status::unavailable;
    if (down) held_buttons_.insert(button_number);
    else held_buttons_.erase(button_number);
    return status::ok;
  }

  status wheel(int dx, int dy) override {
    const int vertical = std::clamp(dy, -10, 10);
    const int horizontal = std::clamp(dx, -10, 10);
    if (vertical != 0) {
      INPUT input{};
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = MOUSEEVENTF_WHEEL;
      input.mi.mouseData = static_cast<DWORD>(-vertical * WHEEL_DELTA);
      if (SendInput(1, &input, sizeof(input)) != 1) return status::unavailable;
    }
    if (horizontal != 0) {
      INPUT input{};
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
      input.mi.mouseData = static_cast<DWORD>(horizontal * WHEEL_DELTA);
      if (SendInput(1, &input, sizeof(input)) != 1) return status::unavailable;
    }
    return status::ok;
  }

  status key(const std::string& code, bool down) override {
    WORD virtual_key = 0;
    bool extended = false;
    if (!resolve_key(code, virtual_key, extended)) return status::rejected;

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtual_key;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = 0;
    if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    if (extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;

    if (SendInput(1, &input, sizeof(input)) != 1) return status::unavailable;
    if (down) held_keys_.insert(virtual_key);
    else held_keys_.erase(virtual_key);
    return status::ok;
  }

  status text(const std::string& utf8) override {
    const std::vector<uint32_t> codepoints = utf8_codepoints(utf8);
    if (codepoints.empty()) return status::rejected;
    std::u16string utf16;
    utf16.reserve(codepoints.size());
    for (uint32_t cp : codepoints) {
      if (cp <= 0xffff) {
        utf16.push_back(static_cast<char16_t>(cp));
      } else {
        cp -= 0x10000;
        utf16.push_back(static_cast<char16_t>(0xd800 + (cp >> 10)));
        utf16.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
      }
    }
    for (char16_t unit : utf16) {
      INPUT input{};
      input.type = INPUT_KEYBOARD;
      input.ki.wVk = 0;
      input.ki.wScan = static_cast<WORD>(unit);
      input.ki.dwFlags = KEYEVENTF_UNICODE;
      if (SendInput(1, &input, sizeof(input)) != 1) {
        return status::unavailable;
      }
      input.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
      if (SendInput(1, &input, sizeof(input)) != 1) {
        return status::unavailable;
      }
    }
    return status::ok;
  }

  void release_all() override {
    for (const int button : held_buttons_) {
      INPUT input{};
      input.type = INPUT_MOUSE;
      if (button <= 3) {
        static const DWORD up_flags[] = {0, MOUSEEVENTF_LEFTUP, MOUSEEVENTF_MIDDLEUP,
                                         MOUSEEVENTF_RIGHTUP};
        input.mi.dwFlags = up_flags[button];
      } else {
        input.mi.dwFlags = MOUSEEVENTF_XUP;
        input.mi.mouseData = button == 4 ? XBUTTON1 : XBUTTON2;
      }
      SendInput(1, &input, sizeof(input));
    }
    held_buttons_.clear();
    for (const WORD key : held_keys_) {
      INPUT input{};
      input.type = INPUT_KEYBOARD;
      input.ki.wVk = key;
      input.ki.dwFlags = KEYEVENTF_KEYUP;
      SendInput(1, &input, sizeof(input));
    }
    held_keys_.clear();
  }

 private:
  std::set<int> held_buttons_;
  std::set<WORD> held_keys_;
  RECT display_{};
  bool has_display_ = false;
};

}  // namespace

std::unique_ptr<input_injector> make_windows_input(std::string&) {
  return std::unique_ptr<input_injector>(new windows_input());
}

}  // namespace molink

#else  // !_WIN32

namespace molink {
std::unique_ptr<input_injector> make_windows_input(std::string& err) {
  err = "built without Windows input support";
  return nullptr;
}
}  // namespace molink

#endif
