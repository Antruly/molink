#include "input_state.h"

#include <algorithm>

namespace molink::input {
namespace {

// The shortcut menu: the Web's KEY_COMBOS minus the bare Win key and Win+L
// (the native client has no lock confirmation dialog yet). Order and key
// sequences are the Web's.
const char* const kWinR[] = {"MetaLeft", "KeyR"};
const char* const kWinE[] = {"MetaLeft", "KeyE"};
const char* const kWinD[] = {"MetaLeft", "KeyD"};
const char* const kWinTab[] = {"MetaLeft", "Tab"};
const char* const kAltTab[] = {"AltLeft", "Tab"};
const char* const kAltF4[] = {"AltLeft", "F4"};
const char* const kCtrlEsc[] = {"ControlLeft", "Escape"};
const char* const kCtrlShiftEsc[] = {"ControlLeft", "ShiftLeft", "Escape"};
const char* const kWinShiftS[] = {"MetaLeft", "ShiftLeft", "KeyS"};
const char* const kPrintScreen[] = {"PrintScreen"};

const key_combo kCombos[] = {
    {"win-r", "Win+R", kWinR, 2},
    {"win-e", "Win+E", kWinE, 2},
    {"win-d", "Win+D", kWinD, 2},
    {"win-tab", "Win+Tab", kWinTab, 2},
    {"alt-tab", "Alt+Tab", kAltTab, 2},
    {"alt-f4", "Alt+F4", kAltF4, 2},
    {"ctrl-esc", "Ctrl+Esc", kCtrlEsc, 2},
    {"ctrl-shift-esc", "Ctrl+Shift+Esc", kCtrlShiftEsc, 3},
    {"win-shift-s", "Win+Shift+S", kWinShiftS, 3},
    {"print-screen", "PrintScreen", kPrintScreen, 1},
};

void add_pressed(std::vector<std::string>& pressed, const std::string& code) {
  if (std::find(pressed.begin(), pressed.end(), code) == pressed.end()) {
    pressed.push_back(code);
  }
}

void drop_pressed(std::vector<std::string>& pressed, const std::string& code) {
  const auto it = std::find(pressed.begin(), pressed.end(), code);
  if (it != pressed.end()) pressed.erase(it);
}

}  // namespace

const key_combo* combos() { return kCombos; }

std::size_t combo_count() { return sizeof(kCombos) / sizeof(kCombos[0]); }

const key_combo* find_combo(std::string_view id) {
  for (std::size_t i = 0; i < combo_count(); ++i) {
    if (id == kCombos[i].id) return &kCombos[i];
  }
  return nullptr;
}

namespace {

/// Layout-dependent fallback for keys that have no portable scan code (or for
/// the `MOLINK_KEYMAP=layout` compatibility path). This is the old VK table.
std::string key_code_vk(int vk, bool extended) {
  if (vk >= vk::K_NUMPAD0 && vk <= vk::K_NUMPAD9) {
    return std::string("Numpad") +
           static_cast<char>('0' + (vk - vk::K_NUMPAD0));
  }

  switch (vk) {
    // NumLock off: Windows reports the keypad as the editing/navigation
    // cluster; the real cluster carries the extended bit (E0). Keep the
    // Numpad* codes so the host's NumLock compensation sees the physical key.
    case vk::K_INSERT: return extended ? "Insert" : "Numpad0";
    case vk::K_END: return extended ? "End" : "Numpad1";
    case vk::K_DOWN: return extended ? "ArrowDown" : "Numpad2";
    case vk::K_NEXT: return extended ? "PageDown" : "Numpad3";
    case vk::K_LEFT: return extended ? "ArrowLeft" : "Numpad4";
    case vk::K_CLEAR: return extended ? "Clear" : "Numpad5";
    case vk::K_RIGHT: return extended ? "ArrowRight" : "Numpad6";
    case vk::K_HOME: return extended ? "Home" : "Numpad7";
    case vk::K_UP: return extended ? "ArrowUp" : "Numpad8";
    case vk::K_PRIOR: return extended ? "PageUp" : "Numpad9";
    case vk::K_DELETE: return extended ? "Delete" : "NumpadDecimal";
    // The keypad Enter is the extended Return; the main one is not.
    case vk::K_RETURN: return extended ? "NumpadEnter" : "Enter";

    // Keypad operators and JIS keys. Windows has a single separator VK; the
    // physical key is the keypad comma.
    case vk::K_MULTIPLY: return "NumpadMultiply";
    case vk::K_ADD: return "NumpadAdd";
    case vk::K_SUBTRACT: return "NumpadSubtract";
    case vk::K_DECIMAL: return "NumpadDecimal";
    case vk::K_DIVIDE: return "NumpadDivide";
    case vk::K_SEPARATOR: return "NumpadComma";
    case vk::K_OEM_NEC_EQUAL: return "NumpadEqual";

    // Main keyboard, modifiers and locks.
    case vk::K_ESCAPE: return "Escape";
    case vk::K_TAB: return "Tab";
    case vk::K_BACK: return "Backspace";
    case vk::K_SPACE: return "Space";
    case vk::K_PAUSE: return "Pause";
    case vk::K_SNAPSHOT: return "PrintScreen";
    case vk::K_CAPITAL: return "CapsLock";
    case vk::K_NUMLOCK: return "NumLock";
    case vk::K_SCROLL: return "ScrollLock";
    case vk::K_SHIFT:
    case vk::K_LSHIFT: return "ShiftLeft";
    case vk::K_RSHIFT: return "ShiftRight";
    case vk::K_LCONTROL: return "ControlLeft";
    case vk::K_RCONTROL: return "ControlRight";
    // Generic modifier messages carry the side in the extended bit.
    case vk::K_CONTROL: return extended ? "ControlRight" : "ControlLeft";
    case vk::K_LMENU: return "AltLeft";
    case vk::K_RMENU: return "AltRight";
    case vk::K_MENU: return extended ? "AltRight" : "AltLeft";
    case vk::K_LWIN: return "MetaLeft";
    case vk::K_RWIN: return "MetaRight";

    // Navigation/editing keys from the extended cluster (the non-extended
    // keypad versions were handled above).
    case vk::K_SELECT: return "Select";
    case vk::K_EXECUTE: return "Execute";
    case vk::K_HELP: return "Help";
    case vk::K_CANCEL: return "Cancel";
    case vk::K_APPS: return "ContextMenu";
    case vk::K_SLEEP: return "Sleep";

    // IME keys. VK_KANA and VK_HANGUL share a value, as do VK_HANJA and
    // VK_KANJI; one DOM code each is all the layout can distinguish.
    case vk::K_CONVERT: return "Convert";
    case vk::K_NONCONVERT: return "NonConvert";
    case vk::K_KANA: return "KanaMode";
    case vk::K_HANJA: return "HanjaMode";

    // Media, volume, browser and launch keys (E0-prefixed, but the code is
    // the same with or without the bit).
    case vk::K_BROWSER_BACK: return "BrowserBack";
    case vk::K_BROWSER_FORWARD: return "BrowserForward";
    case vk::K_BROWSER_REFRESH: return "BrowserRefresh";
    case vk::K_BROWSER_STOP: return "BrowserStop";
    case vk::K_BROWSER_SEARCH: return "BrowserSearch";
    case vk::K_BROWSER_FAVORITES: return "BrowserFavorites";
    case vk::K_BROWSER_HOME: return "BrowserHome";
    case vk::K_VOLUME_MUTE: return "AudioVolumeMute";
    case vk::K_VOLUME_DOWN: return "AudioVolumeDown";
    case vk::K_VOLUME_UP: return "AudioVolumeUp";
    case vk::K_MEDIA_NEXT_TRACK: return "MediaTrackNext";
    case vk::K_MEDIA_PREV_TRACK: return "MediaTrackPrevious";
    case vk::K_MEDIA_STOP: return "MediaStop";
    case vk::K_MEDIA_PLAY_PAUSE: return "MediaPlayPause";
    case vk::K_LAUNCH_MAIL: return "LaunchMail";
    case vk::K_LAUNCH_APP1: return "LaunchApp1";
    case vk::K_LAUNCH_APP2: return "LaunchApp2";

    // OEM punctuation. These are the US positions; a non-US layout still
    // reaches the host through the same VK, which is what the previous
    // native table did.
    case vk::K_OEM_MINUS: return "Minus";
    case vk::K_OEM_PLUS: return "Equal";
    case vk::K_OEM_4: return "BracketLeft";
    case vk::K_OEM_6: return "BracketRight";
    case vk::K_OEM_5: return "Backslash";
    case vk::K_OEM_1: return "Semicolon";
    case vk::K_OEM_7: return "Quote";
    case vk::K_OEM_COMMA: return "Comma";
    case vk::K_OEM_PERIOD: return "Period";
    case vk::K_OEM_2: return "Slash";
    case vk::K_OEM_3: return "Backquote";
    case vk::K_OEM_102: return "IntlBackslash";

    default: break;
  }

  if (vk >= 'A' && vk <= 'Z') {
    return std::string("Key") + static_cast<char>(vk);
  }
  if (vk >= '0' && vk <= '9') {
    return std::string("Digit") + static_cast<char>(vk);
  }
  if (vk >= vk::K_F1 && vk <= vk::K_F24) {
    return "F" + std::to_string(vk - vk::K_F1 + 1);
  }
  return std::string();
}

}  // namespace

std::string key_code(int scan, bool extended, bool numlock_on, int vk) {
  // The physical path never consults NumLock: the scan code describes the
  // position, and Windows reports the same position with NumLock on or off.
  (void)numlock_on;

  // Pause shares its 0x45 position with NumLock; the virtual key is the only
  // thing that tells them apart.
  if (!extended && scan == scan::K_PAUSE && vk == vk::K_PAUSE) {
    return "Pause";
  }

  if (scan != 0) {
    if (extended) {
      switch (scan) {
        case scan::K_RETURN: return "NumpadEnter";
        case scan::K_LCONTROL: return "ControlRight";
        case scan::K_DIVIDE: return "NumpadDivide";
        case scan::K_MULTIPLY: return "PrintScreen";
        case scan::K_LMENU: return "AltRight";
        case scan::K_HOME: return "Home";
        case scan::K_UP: return "ArrowUp";
        case scan::K_PRIOR: return "PageUp";
        case scan::K_LEFT: return "ArrowLeft";
        case scan::K_RIGHT: return "ArrowRight";
        case scan::K_END: return "End";
        case scan::K_DOWN: return "ArrowDown";
        case scan::K_NEXT: return "PageDown";
        case scan::K_INSERT: return "Insert";
        case scan::K_DELETE: return "Delete";
        case scan::K_LWIN: return "MetaLeft";
        case scan::K_RWIN: return "MetaRight";
        case scan::K_APPS: return "ContextMenu";
        case 0x5E: return "Power";
        case scan::K_SLEEP: return "Sleep";
        case 0x63: return "WakeUp";
        case scan::K_MEDIA_PREV_TRACK: return "MediaTrackPrevious";
        case scan::K_MEDIA_NEXT_TRACK: return "MediaTrackNext";
        case scan::K_VOLUME_MUTE: return "AudioVolumeMute";
        case scan::K_LAUNCH_APP2: return "LaunchApp2";
        case scan::K_MEDIA_PLAY_PAUSE: return "MediaPlayPause";
        case scan::K_MEDIA_STOP: return "MediaStop";
        case scan::K_VOLUME_DOWN: return "AudioVolumeDown";
        case scan::K_VOLUME_UP: return "AudioVolumeUp";
        case scan::K_BROWSER_HOME: return "BrowserHome";
        case scan::K_BROWSER_SEARCH: return "BrowserSearch";
        case scan::K_BROWSER_FAVORITES: return "BrowserFavorites";
        case scan::K_BROWSER_REFRESH: return "BrowserRefresh";
        case scan::K_BROWSER_STOP: return "BrowserStop";
        case scan::K_BROWSER_FORWARD: return "BrowserForward";
        case scan::K_BROWSER_BACK: return "BrowserBack";
        case scan::K_LAUNCH_APP1: return "LaunchApp1";
        case scan::K_LAUNCH_MAIL: return "LaunchMail";
        default: break;
      }
    } else {
      // Main letter rows and digits. The rows are positions in scan order,
      // not alphabetical order: 0x2C..0x32 is Z X C V B N M.
      static constexpr char kDigitRow[] = "1234567890";
      static constexpr char kTopRow[] = "QWERTYUIOP";
      static constexpr char kHomeRow[] = "ASDFGHJKL";
      static constexpr char kBottomRow[] = "ZXCVBNM";
      if (scan >= 0x02 && scan <= 0x0B) {
        return std::string("Digit") + kDigitRow[scan - 0x02];
      }
      if (scan >= 0x10 && scan <= 0x19) {
        return std::string("Key") + kTopRow[scan - 0x10];
      }
      if (scan >= 0x1E && scan <= 0x26) {
        return std::string("Key") + kHomeRow[scan - 0x1E];
      }
      if (scan >= 0x2C && scan <= 0x32) {
        return std::string("Key") + kBottomRow[scan - 0x2C];
      }
      if (scan >= scan::K_F1 && scan <= scan::K_F10) {
        return "F" + std::to_string(scan - scan::K_F1 + 1);
      }
      if (scan >= 0x64 && scan <= 0x6B) {
        return "F" + std::to_string(13 + (scan - 0x64));
      }
      switch (scan) {
        case scan::K_ESCAPE: return "Escape";
        case scan::K_OEM_MINUS: return "Minus";
        case scan::K_OEM_PLUS: return "Equal";
        case scan::K_BACK: return "Backspace";
        case scan::K_TAB: return "Tab";
        case scan::K_OEM_4: return "BracketLeft";
        case scan::K_OEM_6: return "BracketRight";
        case scan::K_RETURN: return "Enter";
        case scan::K_LCONTROL: return "ControlLeft";
        case scan::K_OEM_1: return "Semicolon";
        case scan::K_OEM_7: return "Quote";
        case scan::K_OEM_3: return "Backquote";
        case scan::K_LSHIFT: return "ShiftLeft";
        case scan::K_OEM_5: return "Backslash";
        case scan::K_OEM_COMMA: return "Comma";
        case scan::K_OEM_PERIOD: return "Period";
        case scan::K_OEM_2: return "Slash";
        case scan::K_RSHIFT: return "ShiftRight";
        case scan::K_MULTIPLY: return "NumpadMultiply";
        case scan::K_LMENU: return "AltLeft";
        case scan::K_SPACE: return "Space";
        case scan::K_CAPITAL: return "CapsLock";
        case scan::K_NUMLOCK: return "NumLock";
        case scan::K_SCROLL: return "ScrollLock";
        case scan::K_NUMPAD7: return "Numpad7";
        case scan::K_NUMPAD8: return "Numpad8";
        case scan::K_NUMPAD9: return "Numpad9";
        case scan::K_SUBTRACT: return "NumpadSubtract";
        case scan::K_NUMPAD4: return "Numpad4";
        case scan::K_NUMPAD5: return "Numpad5";
        case scan::K_NUMPAD6: return "Numpad6";
        case scan::K_ADD: return "NumpadAdd";
        case scan::K_NUMPAD1: return "Numpad1";
        case scan::K_NUMPAD2: return "Numpad2";
        case scan::K_NUMPAD3: return "Numpad3";
        case scan::K_NUMPAD0: return "Numpad0";
        case scan::K_DECIMAL: return "NumpadDecimal";
        case scan::K_OEM_102: return "IntlBackslash";
        case scan::K_F11: return "F11";
        case scan::K_F12: return "F12";
        // Brazilian ABNT2 reports the keypad comma as VK_SEPARATOR; the JIS
        // NEC keypad reports the same position as VK_OEM_NEC_EQUAL.
        case scan::K_SEPARATOR:
          return vk == vk::K_SEPARATOR ? "NumpadComma" : "NumpadEqual";
        case 0x6C: return "F21";
        case 0x6D: return "F22";
        case 0x6E: return "F23";
        case 0x73: return "IntlRo";
        case scan::K_F24: return "F24";
        case 0x7D: return "IntlYen";
        default: break;
      }
    }
  }

  // Keys with no scan code (or a scan code this OS does not use) keep the
  // old virtual-key path. This is also the `MOLINK_KEYMAP=layout` behaviour.
  if (vk != 0) return key_code_vk(vk, extended);
  return std::string();
}

std::vector<key_event> key_state::key(int scan, bool extended,
                                      bool numlock_on, bool repeat,
                                      bool down, int vk) {
  std::vector<key_event> events;
  const std::string code = key_code(scan, extended, numlock_on, vk);
  if (code.empty()) return events;

  if (down) {
    // Auto-repeat keeps the host's original key-down; a second non-repeat
    // down for a held code would double-press it after a lost key-up.
    if (repeat) return events;
    if (std::find(pressed_.begin(), pressed_.end(), code) != pressed_.end()) {
      return events;
    }
    add_pressed(pressed_, code);
    events.push_back({code, true});
  } else {
    drop_pressed(pressed_, code);
    events.push_back({code, false});
  }
  return events;
}

std::vector<key_event> key_state::combo(const key_combo& combo) {
  std::vector<key_event> events;
  events.reserve(combo.key_count * 2);
  for (std::size_t i = 0; i < combo.key_count; ++i) {
    const std::string code = combo.keys[i];
    add_pressed(pressed_, code);
    events.push_back({code, true});
  }
  for (std::size_t i = combo.key_count; i > 0; --i) {
    const std::string code = combo.keys[i - 1];
    drop_pressed(pressed_, code);
    events.push_back({code, false});
  }
  return events;
}

std::vector<key_event> key_state::release_all() {
  std::vector<key_event> events;
  events.reserve(pressed_.size());
  for (auto it = pressed_.rbegin(); it != pressed_.rend(); ++it) {
    events.push_back({*it, false});
  }
  pressed_.clear();
  return events;
}

void key_state::forget() { pressed_.clear(); }

bool key_state::held(std::string_view code) const {
  for (const std::string& pressed : pressed_) {
    if (pressed.size() == code.size() &&
        std::equal(pressed.begin(), pressed.end(), code.begin())) {
      return true;
    }
  }
  return false;
}

}  // namespace molink::input
