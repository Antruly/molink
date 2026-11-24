// Keyboard state shared by the Windows shell and the tests.
//
// This file deliberately has no Windows dependency: the key translation and
// the pressed-key state machine are the contract with the host, so they are
// built and tested on Linux with the same source the native client compiles.
// The Win32 side only supplies the virtual key, the extended bit and the
// NumLock state; `ui_kit_demo.cpp` static_asserts every virtual-key value
// against <windows.h>.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace molink::input {

/// Windows set-1 scan codes for the keys the native client translates. The
/// position is physical: a German layout puts a different character on the
/// same code, it does not move the key. `lParam` bits 16-23 carry this value.
namespace scan {
inline constexpr int K_ESCAPE = 0x01;
inline constexpr int K_BACK = 0x0E;
inline constexpr int K_TAB = 0x0F;
inline constexpr int K_RETURN = 0x1C;
inline constexpr int K_SPACE = 0x39;
inline constexpr int K_CLEAR = 0x4C;  // keypad 5 position
inline constexpr int K_PAUSE = 0x45;  // shares the NumLock position
inline constexpr int K_CAPITAL = 0x3A;
inline constexpr int K_PRIOR = 0x49;
inline constexpr int K_NEXT = 0x51;
inline constexpr int K_END = 0x4F;
inline constexpr int K_HOME = 0x47;
inline constexpr int K_LEFT = 0x4B;
inline constexpr int K_UP = 0x48;
inline constexpr int K_RIGHT = 0x4D;
inline constexpr int K_DOWN = 0x50;
inline constexpr int K_INSERT = 0x52;
inline constexpr int K_DELETE = 0x53;
inline constexpr int K_LWIN = 0x5B;
inline constexpr int K_RWIN = 0x5C;
inline constexpr int K_APPS = 0x5D;
inline constexpr int K_SLEEP = 0x5F;
inline constexpr int K_NUMPAD0 = 0x52;
inline constexpr int K_NUMPAD1 = 0x4F;
inline constexpr int K_NUMPAD2 = 0x50;
inline constexpr int K_NUMPAD3 = 0x51;
inline constexpr int K_NUMPAD4 = 0x4B;
inline constexpr int K_NUMPAD5 = 0x4C;
inline constexpr int K_NUMPAD6 = 0x4D;
inline constexpr int K_NUMPAD7 = 0x47;
inline constexpr int K_NUMPAD8 = 0x48;
inline constexpr int K_NUMPAD9 = 0x49;
inline constexpr int K_MULTIPLY = 0x37;
inline constexpr int K_ADD = 0x4E;
inline constexpr int K_SEPARATOR = 0x59;
inline constexpr int K_SUBTRACT = 0x4A;
inline constexpr int K_DECIMAL = 0x53;
inline constexpr int K_DIVIDE = 0x35;
inline constexpr int K_F1 = 0x3B;
inline constexpr int K_F10 = 0x44;
inline constexpr int K_F11 = 0x57;
inline constexpr int K_F12 = 0x58;
inline constexpr int K_F13 = 0x64;
inline constexpr int K_F24 = 0x76;
inline constexpr int K_NUMLOCK = 0x45;
inline constexpr int K_SCROLL = 0x46;
inline constexpr int K_LSHIFT = 0x2A;
inline constexpr int K_RSHIFT = 0x36;
inline constexpr int K_LCONTROL = 0x1D;
inline constexpr int K_RCONTROL = 0x1D;
inline constexpr int K_LMENU = 0x38;
inline constexpr int K_RMENU = 0x38;
inline constexpr int K_BROWSER_BACK = 0x6A;
inline constexpr int K_BROWSER_FORWARD = 0x69;
inline constexpr int K_BROWSER_REFRESH = 0x67;
inline constexpr int K_BROWSER_STOP = 0x68;
inline constexpr int K_BROWSER_SEARCH = 0x65;
inline constexpr int K_BROWSER_FAVORITES = 0x66;
inline constexpr int K_BROWSER_HOME = 0x32;
inline constexpr int K_VOLUME_MUTE = 0x20;
inline constexpr int K_VOLUME_DOWN = 0x2E;
inline constexpr int K_VOLUME_UP = 0x30;
inline constexpr int K_MEDIA_NEXT_TRACK = 0x19;
inline constexpr int K_MEDIA_PREV_TRACK = 0x10;
inline constexpr int K_MEDIA_STOP = 0x24;
inline constexpr int K_MEDIA_PLAY_PAUSE = 0x22;
inline constexpr int K_LAUNCH_MAIL = 0x6C;
inline constexpr int K_LAUNCH_APP1 = 0x6B;
inline constexpr int K_LAUNCH_APP2 = 0x21;
inline constexpr int K_OEM_1 = 0x27;      // ;
inline constexpr int K_OEM_PLUS = 0x0D;   // =
inline constexpr int K_OEM_COMMA = 0x33;
inline constexpr int K_OEM_MINUS = 0x0C;
inline constexpr int K_OEM_PERIOD = 0x34;
inline constexpr int K_OEM_2 = 0x35;      // /
inline constexpr int K_OEM_3 = 0x29;      // `
inline constexpr int K_OEM_4 = 0x1A;      // [
inline constexpr int K_OEM_5 = 0x2B;      // backslash
inline constexpr int K_OEM_6 = 0x1B;      // ]
inline constexpr int K_OEM_7 = 0x28;      // '
inline constexpr int K_OEM_102 = 0x56;    // 102nd key
}  // namespace scan

/// Win32 virtual-key values used only by the compatibility path and by keys
/// with no portable scan code (Pause, Help, Select, Execute, Clear, Cancel and
/// the IME conversion keys). The numbers match VK_* exactly (the Windows build
/// checks that with static_assert); keeping them here lets the Linux tests
/// exercise the same fallback.
namespace vk {
inline constexpr int K_CANCEL = 0x03;
inline constexpr int K_BACK = 0x08;
inline constexpr int K_TAB = 0x09;
inline constexpr int K_CLEAR = 0x0C;
inline constexpr int K_RETURN = 0x0D;
inline constexpr int K_SHIFT = 0x10;
inline constexpr int K_CONTROL = 0x11;
inline constexpr int K_MENU = 0x12;
inline constexpr int K_PAUSE = 0x13;
inline constexpr int K_CAPITAL = 0x14;
inline constexpr int K_KANA = 0x15;      // VK_KANA / VK_HANGUL
inline constexpr int K_HANJA = 0x19;     // VK_HANJA / VK_KANJI
inline constexpr int K_ESCAPE = 0x1B;
inline constexpr int K_CONVERT = 0x1C;
inline constexpr int K_NONCONVERT = 0x1D;
inline constexpr int K_SPACE = 0x20;
inline constexpr int K_PRIOR = 0x21;
inline constexpr int K_NEXT = 0x22;
inline constexpr int K_END = 0x23;
inline constexpr int K_HOME = 0x24;
inline constexpr int K_LEFT = 0x25;
inline constexpr int K_UP = 0x26;
inline constexpr int K_RIGHT = 0x27;
inline constexpr int K_DOWN = 0x28;
inline constexpr int K_SELECT = 0x29;
inline constexpr int K_EXECUTE = 0x2B;
inline constexpr int K_SNAPSHOT = 0x2C;
inline constexpr int K_INSERT = 0x2D;
inline constexpr int K_DELETE = 0x2E;
inline constexpr int K_HELP = 0x2F;
inline constexpr int K_LWIN = 0x5B;
inline constexpr int K_RWIN = 0x5C;
inline constexpr int K_APPS = 0x5D;
inline constexpr int K_SLEEP = 0x5F;
inline constexpr int K_NUMPAD0 = 0x60;
inline constexpr int K_NUMPAD9 = 0x69;
inline constexpr int K_MULTIPLY = 0x6A;
inline constexpr int K_ADD = 0x6B;
inline constexpr int K_SEPARATOR = 0x6C;
inline constexpr int K_SUBTRACT = 0x6D;
inline constexpr int K_DECIMAL = 0x6E;
inline constexpr int K_DIVIDE = 0x6F;
inline constexpr int K_F1 = 0x70;
inline constexpr int K_F24 = 0x87;
inline constexpr int K_NUMLOCK = 0x90;
inline constexpr int K_SCROLL = 0x91;
inline constexpr int K_OEM_NEC_EQUAL = 0x92;
inline constexpr int K_LSHIFT = 0xA0;
inline constexpr int K_RSHIFT = 0xA1;
inline constexpr int K_LCONTROL = 0xA2;
inline constexpr int K_RCONTROL = 0xA3;
inline constexpr int K_LMENU = 0xA4;
inline constexpr int K_RMENU = 0xA5;
inline constexpr int K_BROWSER_BACK = 0xA6;
inline constexpr int K_BROWSER_FORWARD = 0xA7;
inline constexpr int K_BROWSER_REFRESH = 0xA8;
inline constexpr int K_BROWSER_STOP = 0xA9;
inline constexpr int K_BROWSER_SEARCH = 0xAA;
inline constexpr int K_BROWSER_FAVORITES = 0xAB;
inline constexpr int K_BROWSER_HOME = 0xAC;
inline constexpr int K_VOLUME_MUTE = 0xAD;
inline constexpr int K_VOLUME_DOWN = 0xAE;
inline constexpr int K_VOLUME_UP = 0xAF;
inline constexpr int K_MEDIA_NEXT_TRACK = 0xB0;
inline constexpr int K_MEDIA_PREV_TRACK = 0xB1;
inline constexpr int K_MEDIA_STOP = 0xB2;
inline constexpr int K_MEDIA_PLAY_PAUSE = 0xB3;
inline constexpr int K_LAUNCH_MAIL = 0xB4;
inline constexpr int K_LAUNCH_APP1 = 0xB6;
inline constexpr int K_LAUNCH_APP2 = 0xB7;
inline constexpr int K_OEM_1 = 0xBA;
inline constexpr int K_OEM_PLUS = 0xBB;
inline constexpr int K_OEM_COMMA = 0xBC;
inline constexpr int K_OEM_MINUS = 0xBD;
inline constexpr int K_OEM_PERIOD = 0xBE;
inline constexpr int K_OEM_2 = 0xBF;
inline constexpr int K_OEM_3 = 0xC0;
inline constexpr int K_OEM_4 = 0xDB;
inline constexpr int K_OEM_5 = 0xDC;
inline constexpr int K_OEM_6 = 0xDD;
inline constexpr int K_OEM_7 = 0xDE;
inline constexpr int K_OEM_102 = 0xE2;
}  // namespace vk

/// One `{"type":"input","k":"key"}` event to send.
struct key_event {
  std::string code;
  bool down = false;
};

/// Translate a physical key position into the browser `KeyboardEvent.code`
/// name the host accepts (`KeyA`, `Numpad7`, `ControlLeft`, ...).
///
/// `scan` is lParam bits 16-23, `extended` is bit 24. The scan code is the
/// position, so a German client and a US client report the same code for the
/// same physical key; the layout only changes the character the host types.
/// The numeric keypad is the reason the extended bit is needed: with NumLock
/// off Windows reports the keypad as Insert/End/Down/..., but the real
/// navigation keys are extended while the keypad ones are not, so the output
/// is `Numpad*` either way.
///
/// `vk` is only a fallback for keys with no portable scan code (Pause, Help,
/// Select, Execute, Clear, Cancel and the IME keys) and for the
/// `MOLINK_KEYMAP=layout` compatibility path. It never decides a position
/// that has a scan code.
///
/// `numlock_on` (GetKeyState(VK_NUMLOCK) & 1) must not change the result. It
/// is part of the signature so callers state the state explicitly and the
/// tests can prove the two paths agree.
///
/// An empty result means the key has no DOM code; drop the event instead of
/// sending a guess.
std::string key_code(int scan, bool extended, bool numlock_on, int vk = 0);

/// One toolbar shortcut, in the same order and with the same keys as the
/// Web's `KEY_COMBOS` (`frontend/src/lib/key-combos.ts`).
struct key_combo {
  const char* id;
  const char* display;
  const char* const* keys;
  std::size_t key_count;
};

const key_combo* combos();
std::size_t combo_count();

/// Looks a shortcut up by its Web id; nullptr when unknown.
const key_combo* find_combo(std::string_view id);

/// Tracks the codes the remote currently has held.
class key_state {
 public:
  /// A physical key event. `scan` is lParam bits 16-23; `repeat` is bit 30 on
  /// a key-down. `vk` is the fallback for keys without a scan code.
  ///
  /// A repeat is dropped, and so is a non-repeat key-down for a code that is
  /// already held (a lost key-up must not turn into a second down). A key-up
  /// is always emitted, even for an unrecorded code: the host may have been
  /// holding it from before this state existed.
  std::vector<key_event> key(int scan, bool extended, bool numlock_on,
                             bool repeat, bool down, int vk = 0);

  /// Modifiers down in the order written, the main key down/up, then the
  /// modifiers up in reverse order (the Web's `keySequence()`). Every event
  /// is emitted and tracked, so a combo cannot leave a key held on the host.
  std::vector<key_event> combo(const key_combo& combo);

  /// Key-up for every held code, newest first, then forget them. Used when
  /// the window loses focus, the session ends, or a local panel takes input.
  std::vector<key_event> release_all();

  /// Forget the held codes without emitting anything (the socket is gone).
  void forget();

  bool held(std::string_view code) const;
  std::size_t held_count() const { return pressed_.size(); }
  const std::vector<std::string>& pressed() const { return pressed_; }

 private:
  std::vector<std::string> pressed_;
};

}  // namespace molink::input
