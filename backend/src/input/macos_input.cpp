#include "input/input.h"

#if defined(__APPLE__)

#include <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

namespace molink {
namespace {

struct NamedKey {
  const char* code;
  CGKeyCode keycode;
};

// ANSI key codes; physical layout, matching KeyboardEvent.code.
const NamedKey kNamedKeys[] = {
    {"KeyA", 0},    {"KeyS", 1},    {"KeyD", 2},    {"KeyF", 3},
    {"KeyH", 4},    {"KeyG", 5},    {"KeyZ", 6},    {"KeyX", 7},
    {"KeyC", 8},    {"KeyV", 9},    {"KeyB", 11},   {"KeyQ", 12},
    {"KeyW", 13},   {"KeyE", 14},   {"KeyR", 15},   {"KeyY", 16},
    {"KeyT", 17},   {"Digit1", 18}, {"Digit2", 19}, {"Digit3", 20},
    {"Digit4", 21}, {"Digit6", 22}, {"Digit5", 23}, {"Equal", 24},
    {"Digit9", 25}, {"Digit7", 26}, {"Minus", 27},  {"Digit8", 28},
    {"Digit0", 29}, {"BracketRight", 30}, {"KeyO", 31}, {"KeyU", 32},
    {"BracketLeft", 33}, {"KeyI", 34}, {"KeyP", 35}, {"Enter", 36},
    {"KeyL", 37},   {"KeyJ", 38},   {"Quote", 39},  {"KeyK", 40},
    {"Semicolon", 41}, {"Backslash", 42}, {"Comma", 43}, {"Slash", 44},
    {"KeyN", 45},   {"KeyM", 46},   {"Period", 47}, {"Tab", 48},
    {"Space", 49},  {"Backquote", 50}, {"Backspace", 51}, {"Escape", 53},
    {"MetaRight", 54}, {"MetaLeft", 55}, {"ShiftLeft", 56}, {"CapsLock", 57},
    {"AltLeft", 58}, {"ControlLeft", 59}, {"ShiftRight", 60}, {"AltRight", 61},
    {"ControlRight", 62}, {"F17", 64}, {"NumpadDecimal", 65}, {"NumpadMultiply", 67},
    {"NumpadAdd", 69}, {"NumLock", 71}, {"NumpadDivide", 75}, {"NumpadEnter", 76},
    {"NumpadSubtract", 78}, {"F18", 79}, {"F19", 80}, {"NumpadEqual", 81},
    {"Numpad0", 82}, {"Numpad1", 83}, {"Numpad2", 84}, {"Numpad3", 85},
    {"Numpad4", 86}, {"Numpad5", 87}, {"Numpad6", 88}, {"Numpad7", 89},
    {"Numpad8", 91}, {"Numpad9", 92}, {"F20", 90}, {"F5", 96}, {"F6", 97},
    {"F7", 98}, {"F3", 99}, {"F8", 100}, {"F9", 101}, {"F11", 103},
    {"F13", 105}, {"F16", 106}, {"F14", 107}, {"F10", 109}, {"F12", 111},
    {"F15", 113}, {"Insert", 114}, {"Home", 115}, {"PageUp", 116},
    {"Delete", 117}, {"F4", 118}, {"End", 119}, {"F2", 120},
    {"PageDown", 121}, {"F1", 122}, {"ArrowLeft", 123}, {"ArrowRight", 124},
    {"ArrowDown", 125}, {"ArrowUp", 126},
};

bool resolve_key(const std::string& code, CGKeyCode& keycode) {
  for (const NamedKey& named : kNamedKeys) {
    if (code == named.code) {
      keycode = named.keycode;
      return true;
    }
  }
  return false;
}

class macos_input final : public input_injector {
 public:
  ~macos_input() override { release_all(); }

  bool available() const override { return true; }
  std::string backend() const override { return "cgevent"; }

  void set_display_rect(int x, int y, int width, int height) override {
    if (width <= 0 || height <= 0) {
      has_display_ = false;
      return;
    }
    display_bounds_ = CGRectMake(static_cast<CGFloat>(x),
                                 static_cast<CGFloat>(y),
                                 static_cast<CGFloat>(width),
                                 static_cast<CGFloat>(height));
    has_display_ = true;
  }

  status move(double nx, double ny) override {
    if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) return status::rejected;
    const CGRect bounds =
        has_display_ ? display_bounds_ : CGDisplayBounds(CGMainDisplayID());
    if (bounds.size.width <= 0 || bounds.size.height <= 0) {
      return status::unavailable;
    }
    const CGPoint point =
        CGPointMake(bounds.origin.x + nx * bounds.size.width,
                    bounds.origin.y + ny * bounds.size.height);
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, point,
                                               kCGMouseButtonLeft);
    if (event == nullptr) return status::unavailable;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return status::ok;
  }

  status button(int button_number, bool down) override {
    if (button_number < 1 || button_number > 3) return status::rejected;
    CGEventType type = kCGEventLeftMouseDown;
    CGMouseButton button = kCGMouseButtonLeft;
    if (button_number == 2) {
      type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
      button = kCGMouseButtonCenter;
    } else if (button_number == 3) {
      type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
      button = kCGMouseButtonRight;
    } else {
      type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
    }
    const CGPoint point = current_pointer();
    CGEventRef event =
        CGEventCreateMouseEvent(nullptr, type, point, button);
    if (event == nullptr) return status::unavailable;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    if (down) held_buttons_.insert(button_number);
    else held_buttons_.erase(button_number);
    return status::ok;
  }

  status wheel(int dx, int dy) override {
    const int vertical = std::clamp(dy, -10, 10);
    const int horizontal = std::clamp(dx, -10, 10);
    if (vertical == 0 && horizontal == 0) return status::ok;
    CGEventRef event = CGEventCreateScrollWheelEvent(
        nullptr, kCGScrollEventUnitLine, 2, -vertical, horizontal);
    if (event == nullptr) return status::unavailable;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return status::ok;
  }

  status key(const std::string& code, bool down) override {
    CGKeyCode keycode = 0;
    if (!resolve_key(code, keycode)) return status::rejected;
    CGEventRef event = CGEventCreateKeyboardEvent(nullptr, keycode, down);
    if (event == nullptr) return status::unavailable;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    if (down) held_keys_.insert(keycode);
    else held_keys_.erase(keycode);
    return status::ok;
  }

  void release_all() override {
    for (const int button : held_buttons_) {
      CGEventType type = kCGEventLeftMouseUp;
      CGMouseButton kind = kCGMouseButtonLeft;
      if (button == 2) {
        type = kCGEventOtherMouseUp;
        kind = kCGMouseButtonCenter;
      } else if (button == 3) {
        type = kCGEventRightMouseUp;
        kind = kCGMouseButtonRight;
      }
      CGEventRef event =
          CGEventCreateMouseEvent(nullptr, type, current_pointer(), kind);
      if (event != nullptr) {
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
      }
    }
    held_buttons_.clear();
    for (const CGKeyCode keycode : held_keys_) {
      CGEventRef event = CGEventCreateKeyboardEvent(nullptr, keycode, false);
      if (event != nullptr) {
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
      }
    }
    held_keys_.clear();
  }

 private:
  static CGPoint current_pointer() {
    CGEventRef event = CGEventCreate(nullptr);
    if (event == nullptr) return CGPointMake(0, 0);
    const CGPoint point = CGEventGetLocation(event);
    CFRelease(event);
    return point;
  }

  std::set<int> held_buttons_;
  std::set<CGKeyCode> held_keys_;
  CGRect display_bounds_{};
  bool has_display_ = false;
};

}  // namespace

std::unique_ptr<input_injector> make_macos_input(std::string&) {
  return std::unique_ptr<input_injector>(new macos_input());
}

}  // namespace molink

#else  // !__APPLE__

namespace molink {
std::unique_ptr<input_injector> make_macos_input(std::string& err) {
  err = "built without macOS input support";
  return nullptr;
}
}  // namespace molink

#endif
