#include "input/input.h"
#include "input/keymap.h"

#if defined(__APPLE__)

#include <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "util.h"

namespace molink {
namespace {

// The ANSI/ISO/JIS key-code table lives in keymap.cpp so the same data is
// unit-tested on every platform.

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
    if (button_number < 1 || button_number > 5) return status::rejected;
    CGEventType type = kCGEventLeftMouseDown;
    CGMouseButton button = kCGMouseButtonLeft;
    if (button_number == 2) {
      type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
      button = kCGMouseButtonCenter;
    } else if (button_number == 3) {
      type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
      button = kCGMouseButtonRight;
    } else if (button_number >= 4) {
      // Side buttons: the protocol's 4/5 are back/forward, which CoreGraphics
      // numbers 3/4 as "other" mouse buttons.
      type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
      button = static_cast<CGMouseButton>(button_number - 1);
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
    const key_lookup mapping = lookup_key(keymap_platform::macos, code);
    // An unsupported code (for example PrintScreen, which has no Carbon key
    // code) still rejects with the key's name instead of being dropped.
    if (!mapping.found || mapping.unsupported) return status::rejected;
    // macOS keypad keys always produce digits; there is no NumLock state to
    // compensate for, unlike Windows and X11.
    const CGKeyCode keycode = static_cast<CGKeyCode>(mapping.value);
    CGEventRef event = CGEventCreateKeyboardEvent(nullptr, keycode, down);
    if (event == nullptr) return status::unavailable;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    if (down) held_keys_.insert(keycode);
    else held_keys_.erase(keycode);
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

    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
    if (down == nullptr) return status::unavailable;
    CGEventKeyboardSetUnicodeString(
        down, static_cast<UniCharCount>(utf16.size()),
        reinterpret_cast<const UniChar*>(utf16.data()));
    CGEventPost(kCGHIDEventTap, down);
    CFRelease(down);

    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
    if (up != nullptr) {
      CGEventKeyboardSetUnicodeString(
          up, static_cast<UniCharCount>(utf16.size()),
          reinterpret_cast<const UniChar*>(utf16.data()));
      CGEventPost(kCGHIDEventTap, up);
      CFRelease(up);
    }
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
