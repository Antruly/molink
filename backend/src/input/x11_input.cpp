#include "input/input.h"

#if MOLINK_HAVE_X11

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

namespace molink {
namespace {

/// libXtst ships as a runtime library without headers on many systems.
/// XTEST's entry points are stable across 1.x, so resolve them dynamically.
using XTestFakeMotionEventFn = int (*)(Display*, int, int, int, unsigned long);
using XTestFakeButtonEventFn = int (*)(Display*, unsigned int, int,
                                       unsigned long);
using XTestFakeKeyEventFn = int (*)(Display*, unsigned int, int,
                                    unsigned long);
using XTestQueryExtensionFn = int (*)(Display*, int*, int*, int*, int*);

bool starts_with(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}

/// Map a browser KeyboardEvent.code to an X keysym name. The browser sends the
/// physical key, so the base (unshifted) symbol is correct: modifiers are
/// injected as separate key events, and the X server applies them.
std::string keysym_name(const std::string& code) {
  if (code.size() == 4 && starts_with(code, "Key")) {
    return std::string(1, static_cast<char>(code[3] - 'A' + 'a'));
  }
  if (code.size() == 6 && starts_with(code, "Digit")) {
    return std::string(1, code[5]);
  }

  struct Named {
    const char* code;
    const char* keysym;
  };
  static const Named kNamed[] = {
      {"Enter", "Return"},        {"NumpadEnter", "KP_Enter"},
      {"Backspace", "BackSpace"}, {"Tab", "Tab"},
      {"Space", "space"},         {"Escape", "Escape"},
      {"Delete", "Delete"},       {"Insert", "Insert"},
      {"Home", "Home"},           {"End", "End"},
      {"PageUp", "Prior"},        {"PageDown", "Next"},
      {"ArrowLeft", "Left"},      {"ArrowRight", "Right"},
      {"ArrowUp", "Up"},          {"ArrowDown", "Down"},
      {"ShiftLeft", "Shift_L"},   {"ShiftRight", "Shift_R"},
      {"ControlLeft", "Control_L"}, {"ControlRight", "Control_R"},
      {"AltLeft", "Alt_L"},       {"AltRight", "Alt_R"},
      {"MetaLeft", "Super_L"},    {"MetaRight", "Super_R"},
      {"CapsLock", "Caps_Lock"},  {"NumLock", "Num_Lock"},
      {"ScrollLock", "Scroll_Lock"}, {"PrintScreen", "Print"},
      {"Pause", "Pause"},         {"ContextMenu", "Menu"},
      {"Minus", "minus"},         {"Equal", "equal"},
      {"BracketLeft", "bracketleft"}, {"BracketRight", "bracketright"},
      {"Backslash", "backslash"}, {"Semicolon", "semicolon"},
      {"Quote", "apostrophe"},    {"Backquote", "grave"},
      {"Comma", "comma"},         {"Period", "period"},
      {"Slash", "slash"},
  };
  for (const Named& n : kNamed) {
    if (code == n.code) return n.keysym;
  }
  if (code.size() >= 2 && code[0] == 'F') {
    // F1..F24
    const std::string digits = code.substr(1);
    if (!digits.empty() &&
        std::all_of(digits.begin(), digits.end(),
                    [](char c) { return c >= '0' && c <= '9'; })) {
      return code;
    }
  }
  return std::string();
}

class x11_input final : public input_injector {
 public:
  static std::unique_ptr<x11_input> create(std::string& err) {
    std::unique_ptr<x11_input> input(new x11_input());
    if (!input->init(err)) return nullptr;
    return input;
  }

  ~x11_input() override {
    release_all();
    if (display_ != nullptr) XCloseDisplay(display_);
    if (lib_ != nullptr) dlclose(lib_);
  }

  bool available() const override { return display_ != nullptr; }
  std::string backend() const override { return "xtest"; }

  status move(double nx, double ny) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (display_ == nullptr) return status::unavailable;
    if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) {
      return status::rejected;
    }
    const int width = DisplayWidth(display_, screen_);
    const int height = DisplayHeight(display_, screen_);
    if (width <= 0 || height <= 0) return status::unavailable;
    const int x = static_cast<int>(std::lround(nx * (width - 1)));
    const int y = static_cast<int>(std::lround(ny * (height - 1)));
    fake_motion_(display_, screen_, x, y, CurrentTime);
    XFlush(display_);
    return status::ok;
  }

  status button(int button_number, bool down) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (display_ == nullptr) return status::unavailable;
    if (button_number < 1 || button_number > 7) return status::rejected;
    fake_button_(display_, static_cast<unsigned>(button_number), down ? True : False,
                 CurrentTime);
    XFlush(display_);
    if (down) held_buttons_.insert(button_number);
    else held_buttons_.erase(button_number);
    return status::ok;
  }

  status wheel(int dx, int dy) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (display_ == nullptr) return status::unavailable;
    // X11 maps wheel notches to buttons 4/5 (vertical) and 6/7 (horizontal).
    const int vertical = std::clamp(dy, -10, 10);
    const int horizontal = std::clamp(dx, -10, 10);
    for (int i = 0; i < std::abs(vertical); ++i) {
      const unsigned b = vertical > 0 ? 5 : 4;
      fake_button_(display_, b, True, CurrentTime);
      fake_button_(display_, b, False, CurrentTime);
    }
    for (int i = 0; i < std::abs(horizontal); ++i) {
      const unsigned b = horizontal > 0 ? 7 : 6;
      fake_button_(display_, b, True, CurrentTime);
      fake_button_(display_, b, False, CurrentTime);
    }
    XFlush(display_);
    return status::ok;
  }

  status key(const std::string& code, bool down) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (display_ == nullptr) return status::unavailable;
    const std::string name = keysym_name(code);
    if (name.empty()) return status::rejected;
    const KeySym symbol = XStringToKeysym(name.c_str());
    if (symbol == NoSymbol) return status::rejected;
    const KeyCode keycode = XKeysymToKeycode(display_, symbol);
    if (keycode == 0) return status::rejected;
    fake_key_(display_, keycode, down ? True : False, CurrentTime);
    XFlush(display_);
    if (down) held_keys_.insert(keycode);
    else held_keys_.erase(keycode);
    return status::ok;
  }

  void release_all() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (display_ == nullptr) return;
    for (const int b : held_buttons_) {
      fake_button_(display_, static_cast<unsigned>(b), False, CurrentTime);
    }
    held_buttons_.clear();
    for (const KeyCode k : held_keys_) {
      fake_key_(display_, k, False, CurrentTime);
    }
    held_keys_.clear();
    XFlush(display_);
  }

 private:
  x11_input() = default;

  bool init(std::string& err) {
    lib_ = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (lib_ == nullptr) {
      err = "libXtst.so.6 is not available";
      return false;
    }
    fake_motion_ = reinterpret_cast<XTestFakeMotionEventFn>(
        dlsym(lib_, "XTestFakeMotionEvent"));
    fake_button_ = reinterpret_cast<XTestFakeButtonEventFn>(
        dlsym(lib_, "XTestFakeButtonEvent"));
    fake_key_ =
        reinterpret_cast<XTestFakeKeyEventFn>(dlsym(lib_, "XTestFakeKeyEvent"));
    auto query = reinterpret_cast<XTestQueryExtensionFn>(
        dlsym(lib_, "XTestQueryExtension"));
    if (fake_motion_ == nullptr || fake_button_ == nullptr ||
        fake_key_ == nullptr || query == nullptr) {
      err = "libXtst is missing the XTEST entry points";
      return false;
    }

    const char* requested = std::getenv("MOLINK_DISPLAY");
    display_ = XOpenDisplay(requested);
    if (display_ == nullptr) {
      err = "cannot open an X display for input injection";
      return false;
    }
    screen_ = DefaultScreen(display_);

    int event_base = 0;
    int error_base = 0;
    int major = 0;
    int minor = 0;
    if (!query(display_, &event_base, &error_base, &major, &minor)) {
      err = "the X server does not support XTEST";
      return false;
    }
    return true;
  }

  void* lib_ = nullptr;
  Display* display_ = nullptr;
  int screen_ = 0;
  XTestFakeMotionEventFn fake_motion_ = nullptr;
  XTestFakeButtonEventFn fake_button_ = nullptr;
  XTestFakeKeyEventFn fake_key_ = nullptr;
  std::mutex mutex_;
  std::set<int> held_buttons_;
  std::set<unsigned> held_keys_;
};

}  // namespace

std::unique_ptr<input_injector> make_x11_input(std::string& err) {
  return x11_input::create(err);
}

}  // namespace molink

#else  // !MOLINK_HAVE_X11

namespace molink {
std::unique_ptr<input_injector> make_x11_input(std::string& err) {
  err = "built without X11 input support";
  return nullptr;
}
}  // namespace molink

#endif
