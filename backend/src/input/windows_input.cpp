#include "input/input.h"
#include "input/keymap.h"

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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "util.h"
#include "input/key_alias.h"
#include "input/text_encoding.h"

namespace molink {
namespace {

// A held key is identified by its physical position (scan code + E0 bit).
// Keys without a scan code fall back to the virtual key, which is the only
// thing that can describe them.
struct held_key {
  unsigned int scan;
  unsigned int vk;
  bool extended;
  bool operator<(const held_key& other) const {
    if (scan != other.scan) return scan < other.scan;
    if (vk != other.vk) return vk < other.vk;
    return extended < other.extended;
  }
};

INPUT key_input(unsigned int vk, unsigned int scan, bool extended, bool down) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  if (scan != 0) {
    // The scan code is the physical key. wVk is filled from it only so the
    // applications that read the virtual key see a consistent value; the
    // layout still decides which character the key produces.
    input.ki.wVk =
        static_cast<WORD>(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX));
    input.ki.wScan = static_cast<WORD>(scan);
    input.ki.dwFlags |= KEYEVENTF_SCANCODE;
  } else {
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.wScan = static_cast<WORD>(
        MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
  }
  if (extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
  return input;
}

/// `MOLINK_KEYMAP=layout` restores the old virtual-key path, where the host
/// layout decides what a code means.
bool layout_keymap_mode() {
  const char* mode = std::getenv("MOLINK_KEYMAP");
  return mode != nullptr && std::strcmp(mode, "layout") == 0;
}

/// NumLock toggle of the console session.
///
/// GetKeyState reports the state as of the last message this thread removed
/// from its queue. The core's I/O thread never pumps messages, so that value
/// can be frozen at thread start; GetAsyncKeyState reads the live toggle. Use
/// it first and fall back to GetKeyState when it has nothing to say.
bool read_numlock_raw() {
  const SHORT async_state = GetAsyncKeyState(VK_NUMLOCK);
  if (async_state != 0) return (async_state & 0x0001) != 0;
  return (GetKeyState(VK_NUMLOCK) & 0x0001) != 0;
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
    const key_lookup mapping = lookup_key(keymap_platform::windows, code);
    // Unsupported codes (Again/Undo/Cut/Copy/Paste/Find) and unknown codes
    // both reject: the host names the key in `bad_input`, it never drops it.
    if (!mapping.found || mapping.unsupported) return status::rejected;

    // Physical injection uses the scan code. The compatibility switch and
    // the few keys with no portable scan code go through the virtual key.
    const unsigned int scan =
        layout_keymap_ ? (mapping.value == 0 ? mapping.scan : 0) : mapping.scan;
    const unsigned int vk = mapping.value;
    const held_key id{scan != 0 ? scan : vk, vk, mapping.extended};
    const bool numlock_sensitive = key_depends_on_numlock(code);
    const bool numlock_event =
        canonical_key_code(code) == "NumLock";
    // Read before the key-down: an explicit NumLock press toggles whatever the
    // state was. The key-up does not toggle again.
    const bool numlock_before =
        (numlock_event && down) ? numlock_state() : false;

    // An explicit NumLock press hands control back to the client: do not undo
    // the user's toggle when the keypad key is released.
    if (numlock_event) {
      numlock_forced_ = false;
      numlock_held_.clear();
    }

    if (down && numlock_sensitive && !numlock_state()) {
      // A Windows keypad digit with NumLock off arrives as End/Down/... .
      // Turn NumLock on for the key press and restore the host state when the
      // last keypad key is released.
      if (tap_numlock(true)) {
        numlock_forced_ = true;
        std::fprintf(stderr,
                     "input: numlock was off; enabled it for %s (restored on "
                     "release)\n",
                     code.c_str());
      }
    }
    if (numlock_sensitive) {
      if (down) numlock_held_.insert(static_cast<WORD>(id.scan));
      else numlock_held_.erase(static_cast<WORD>(id.scan));
    }

    INPUT input = key_input(vk, scan, mapping.extended, down);
    if (SendInput(1, &input, sizeof(input)) != 1) return status::unavailable;
    if (down) held_keys_.insert(id);
    else held_keys_.erase(id);
    if (numlock_event && down) {
      // The injected key-down toggled the host state; remember the expected
      // result instead of trusting an async read that may not have caught up.
      numlock_shadow_ = !numlock_before;
      numlock_shadow_valid_ = true;
      numlock_changed_at_ = GetTickCount64();
    }

    if (!down && numlock_sensitive && numlock_held_.empty() && numlock_forced_) {
      // Only restore if the compensation really took effect; tapping a
      // NumLock that is already off would turn it on.
      if (numlock_state()) tap_numlock(false);
      numlock_forced_ = false;
      std::fprintf(stderr,
                   "input: numlock restored to off after keypad input\n");
    }
    return status::ok;
  }

  status text(const std::string& utf8) override {
    const std::vector<uint32_t> codepoints = utf8_codepoints(utf8);
    if (codepoints.empty()) return status::rejected;
    // KEYEVENTF_UNICODE sends UTF-16 code units, so astral characters (emoji,
    // some CJK extensions) are split into a surrogate pair.
    const std::u16string utf16 = utf16_units(codepoints);
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
    for (const held_key& key : held_keys_) {
      INPUT input = key_input(key.vk, key.scan, key.extended, false);
      SendInput(1, &input, sizeof(input));
    }
    held_keys_.clear();
    // Never strand the host with NumLock on because a client disconnected
    // while a keypad key was held.
    if (numlock_forced_) {
      if (numlock_state()) tap_numlock(false);
      numlock_forced_ = false;
      std::fprintf(stderr,
                   "input: numlock restored to off after input release\n");
    }
    numlock_held_.clear();
  }

 private:
  /// The toggle as this injector last saw or set it.
  ///
  /// GetAsyncKeyState can lag a SendInput toggle by a few milliseconds. A
  /// burst of keypad keys then reads the old value and skips the
  /// compensation for every other key. Trust our own writes for a short
  /// window, and re-read the OS state afterwards so a physical NumLock press
  /// is still noticed.
  bool numlock_state() {
    const ULONGLONG now = GetTickCount64();
    if (numlock_shadow_valid_ && now - numlock_changed_at_ < 1000) {
      return numlock_shadow_;
    }
    numlock_shadow_ = read_numlock_raw();
    numlock_shadow_valid_ = true;
    return numlock_shadow_;
  }

  /// Tap NumLock so the host ends in @p target_on, then remember that result.
  bool tap_numlock(bool target_on) {
    INPUT inputs[2] = {key_input(VK_NUMLOCK, 0x45, true, true),
                       key_input(VK_NUMLOCK, 0x45, true, false)};
    if (SendInput(2, inputs, sizeof(INPUT)) != 2) return false;
    numlock_shadow_ = target_on;
    numlock_shadow_valid_ = true;
    numlock_changed_at_ = GetTickCount64();
    // Let the input thread apply the toggle before the keypad key follows.
    Sleep(1);
    return true;
  }

  std::set<int> held_buttons_;
  std::set<held_key> held_keys_;
  std::set<WORD> numlock_held_;
  bool numlock_forced_ = false;
  bool numlock_shadow_ = false;
  bool numlock_shadow_valid_ = false;
  ULONGLONG numlock_changed_at_ = 0;
  RECT display_{};
  bool has_display_ = false;
  bool layout_keymap_ = layout_keymap_mode();
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
