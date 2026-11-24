#include "input/input.h"

#if MOLINK_HAVE_X11

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "util.h"
#include "input/keymap.h"
#include "input/text_encoding.h"

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

/// Map a browser KeyboardEvent.code to an X keysym name. The browser sends the
/// physical key, so the base (unshifted) symbol is correct: modifiers are
/// injected as separate key events, and the X server applies them. The table
/// lives in keymap.cpp so the same data is unit-tested on every platform.

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

  void set_display_rect(int x, int y, int width, int height) override {
    std::lock_guard<std::mutex> lock(mutex_);
    display_x_ = x;
    display_y_ = y;
    display_w_ = width;
    display_h_ = height;
  }

  status move(double nx, double ny) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (display_ == nullptr) return status::unavailable;
    if (nx < 0.0 || nx > 1.0 || ny < 0.0 || ny > 1.0) {
      return status::rejected;
    }
    const int width =
        display_w_ > 0 ? display_w_ : DisplayWidth(display_, screen_);
    const int height =
        display_h_ > 0 ? display_h_ : DisplayHeight(display_, screen_);
    if (width <= 0 || height <= 0) return status::unavailable;
    const int x =
        display_x_ + static_cast<int>(std::lround(nx * (width - 1)));
    const int y =
        display_y_ + static_cast<int>(std::lround(ny * (height - 1)));
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
    const key_lookup mapping = lookup_key(keymap_platform::x11, code);
    // Unknown and explicitly unsupported codes both reject, so the host can
    // name the key in its `bad_input` reply instead of dropping it.
    if (!mapping.found || mapping.unsupported) return status::rejected;

    KeyCode keycode = 0;
    if (layout_keymap_) {
      // Compatibility path: resolve the character through the host layout.
      if (mapping.keysym.empty()) return status::rejected;
      const KeySym symbol = XStringToKeysym(mapping.keysym.c_str());
      if (symbol == NoSymbol) return status::rejected;
      keycode = XKeysymToKeycode(display_, symbol);
    } else {
      // Physical path: evdev + 8 is the position in the X server's keymap,
      // so a German or AZERTY layout does not move the key.
      keycode = static_cast<KeyCode>(mapping.keycode);
    }
    if (keycode == 0) return status::rejected;

    const bool numlock_sensitive = key_depends_on_numlock(code);
    // An explicit NumLock press owns the state from here on.
    if (keycode == numlock_keycode_) {
      numlock_forced_ = false;
      numlock_held_.clear();
    }
    if (down && numlock_sensitive && numlock_keycode_ != 0 && !numlock_on()) {
      // With NumLock off the keypad keycode resolves to KP_End/KP_Down/... .
      // Turn NumLock on for the press and restore the host state on release.
      tap_numlock();
      numlock_forced_ = true;
      std::fprintf(stderr,
                   "input: numlock was off; enabled it for %s (restored on "
                   "release)\n",
                   code.c_str());
    }
    if (numlock_sensitive) {
      if (down) numlock_held_.insert(keycode);
      else numlock_held_.erase(keycode);
    }

    fake_key_(display_, keycode, down ? True : False, CurrentTime);
    XFlush(display_);
    if (down) held_keys_.insert(keycode);
    else held_keys_.erase(keycode);

    if (!down && numlock_sensitive && numlock_held_.empty() && numlock_forced_) {
      if (numlock_on()) tap_numlock();
      numlock_forced_ = false;
      std::fprintf(stderr,
                   "input: numlock restored to off after keypad input\n");
    }
    return status::ok;
  }

  status text(const std::string& utf8) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (display_ == nullptr) return status::unavailable;
    const std::vector<uint32_t> codepoints = utf8_codepoints(utf8);
    if (codepoints.empty()) return status::rejected;

    int min_keycode = 0;
    int max_keycode = 0;
    XDisplayKeycodes(display_, &min_keycode, &max_keycode);
    if (max_keycode <= min_keycode) return status::unavailable;
    const KeyCode spare = static_cast<KeyCode>(max_keycode);

    int per_keycode = 0;
    KeySym* original = XGetKeyboardMapping(display_, spare, 1, &per_keycode);

    for (uint32_t cp : codepoints) {
      const KeySym symbol = x11_unicode_keysym(cp);
      if (symbol == NoSymbol) continue;
      KeySym symbols[2] = {symbol, symbol};
      XChangeKeyboardMapping(display_, spare, 2, symbols, 1);
      XSync(display_, False);
      fake_key_(display_, spare, True, CurrentTime);
      fake_key_(display_, spare, False, CurrentTime);
      XSync(display_, False);
    }

    if (original != nullptr && per_keycode > 0) {
      XChangeKeyboardMapping(display_, spare, per_keycode, original, 1);
      XFree(original);
    } else {
      KeySym none[2] = {NoSymbol, NoSymbol};
      XChangeKeyboardMapping(display_, spare, 2, none, 1);
    }
    XSync(display_, False);
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
    // Never strand the host with NumLock on because the client disconnected
    // while a keypad key was held.
    if (numlock_forced_) {
      if (numlock_on()) tap_numlock();
      numlock_forced_ = false;
      std::fprintf(stderr,
                   "input: numlock restored to off after input release\n");
    }
    numlock_held_.clear();
    XFlush(display_);
  }

 private:
  x11_input() = default;

  /// One-time check that the server's keymap really uses the evdev positions.
  /// Only the keys every desktop has are checked: a server that remaps them
  /// would make physical injection type the wrong characters, so it keeps the
  /// old layout lookup instead. Media/extra keys are not a reason to abandon
  /// physical mapping.
  bool validate_static_keymap() {
    int min_keycode = 0;
    int max_keycode = 0;
    XDisplayKeycodes(display_, &min_keycode, &max_keycode);
    if (max_keycode < min_keycode) return false;
    int per_keycode = 0;
    KeySym* map = XGetKeyboardMapping(
        display_, static_cast<KeyCode>(min_keycode),
        max_keycode - min_keycode + 1, &per_keycode);
    if (map == nullptr || per_keycode <= 0) {
      if (map != nullptr) XFree(map);
      return false;
    }
    static const char* const kCoreCodes[] = {
        "KeyA",   "KeyM",    "KeyY",       "KeyZ",     "Digit1",
        "Digit0", "Escape",  "Enter",      "Tab",      "Space",
        "ShiftLeft", "ControlLeft", "AltLeft", "ArrowLeft", "Backslash",
        "Semicolon", "IntlBackslash"};
    bool valid = true;
    for (const char* code : kCoreCodes) {
      const key_lookup mapping = lookup_key(keymap_platform::x11, code);
      if (!mapping.found || mapping.unsupported ||
          mapping.keycode < static_cast<unsigned int>(min_keycode) ||
          mapping.keycode > static_cast<unsigned int>(max_keycode)) {
        valid = false;
        break;
      }
      const KeySym first =
          map[(mapping.keycode - static_cast<unsigned int>(min_keycode)) *
              static_cast<unsigned int>(per_keycode)];
      if (first == NoSymbol) {
        valid = false;
        break;
      }
    }
    XFree(map);
    return valid;
  }

  /// NumLock is a server lock modifier. Find the modifier bit it occupies so
  /// the state can be queried without assuming Mod2 (layouts differ).
  bool numlock_on() {
    if (numlock_mask_ == 0) return true;
    Window root = None;
    Window child = None;
    int root_x = 0;
    int root_y = 0;
    int win_x = 0;
    int win_y = 0;
    unsigned int state = 0;
    if (!XQueryPointer(display_, DefaultRootWindow(display_), &root, &child,
                       &root_x, &root_y, &win_x, &win_y, &state)) {
      return true;  // cannot tell; do not toggle the host state blindly
    }
    return (state & numlock_mask_) != 0;
  }

  void tap_numlock() {
    fake_key_(display_, numlock_keycode_, True, CurrentTime);
    fake_key_(display_, numlock_keycode_, False, CurrentTime);
    XSync(display_, False);
  }

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

    // The default path injects physical X key codes (evdev + 8), which do not
    // move when the host layout changes. `MOLINK_KEYMAP=layout` restores the
    // old character-based lookup through the host's current layout.
    const char* keymap_mode = std::getenv("MOLINK_KEYMAP");
    layout_keymap_ =
        keymap_mode != nullptr && std::strcmp(keymap_mode, "layout") == 0;
    if (!layout_keymap_ && !validate_static_keymap()) {
      layout_keymap_ = true;
      std::fprintf(stderr,
                   "input: X server key codes do not match the evdev layout; "
                   "falling back to layout lookup\n");
    }

    numlock_keycode_ =
        layout_keymap_
            ? XKeysymToKeycode(display_, XK_Num_Lock)
            : static_cast<KeyCode>(
                  lookup_key(keymap_platform::x11, "NumLock").keycode);
    if (numlock_keycode_ != 0) {
      if (XModifierKeymap* map = XGetModifierMapping(display_)) {
        for (int mod = 0; mod < 8; ++mod) {
          for (int key = 0; key < map->max_keypermod; ++key) {
            if (map->modifiermap[mod * map->max_keypermod + key] ==
                numlock_keycode_) {
              numlock_mask_ |= 1u << mod;
            }
          }
        }
        XFreeModifiermap(map);
      }
    }

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
  int display_x_ = 0;
  int display_y_ = 0;
  int display_w_ = 0;
  int display_h_ = 0;
  XTestFakeMotionEventFn fake_motion_ = nullptr;
  XTestFakeButtonEventFn fake_button_ = nullptr;
  XTestFakeKeyEventFn fake_key_ = nullptr;
  std::mutex mutex_;
  std::set<int> held_buttons_;
  std::set<unsigned> held_keys_;
  KeyCode numlock_keycode_ = 0;
  unsigned int numlock_mask_ = 0;
  bool layout_keymap_ = false;
  std::set<KeyCode> numlock_held_;
  bool numlock_forced_ = false;
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
