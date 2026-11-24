// Coverage test for the host keyboard tables.
//
// The same table is used by every platform injector, so it is checked here
// without a display, a window or an OS keyboard API. Every canonical
// KeyboardEvent.code must resolve to a native key or be explicitly marked
// unsupported; a missing entry is the "the keypad does nothing" bug this test
// exists to prevent.
//
// The Windows and X11 values are physical positions (scan code, evdev + 8).
// A layout decides which character a position produces; it never changes the
// number, which is what the layout-independence checks pin down.
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "input/keymap.h"
#include "input/text_encoding.h"

namespace {

using molink::key_lookup;
using molink::keymap_platform;

int failures = 0;

void expect(bool condition, const char* what, const std::string& detail) {
  if (condition) return;
  std::fprintf(stderr, "FAIL: %s (%s)\n", what, detail.c_str());
  ++failures;
}

std::string platform_name(keymap_platform platform) {
  switch (platform) {
    case keymap_platform::windows:
      return "windows";
    case keymap_platform::x11:
      return "x11";
    case keymap_platform::macos:
      return "macos";
  }
  return "?";
}

void check_platform(keymap_platform platform) {
  const std::string name = platform_name(platform);
  for (std::size_t i = 0; i < molink::kCanonicalKeyCodeCount; ++i) {
    const std::string code = molink::kCanonicalKeyCodes[i];
    const key_lookup resolved = molink::lookup_key(platform, code);
    expect(resolved.found, "code is known to the platform",
           name + " " + code);
    if (!resolved.found) continue;
    bool has_native = resolved.unsupported;
    switch (platform) {
      case keymap_platform::windows:
        has_native = has_native || resolved.value != 0 || resolved.scan != 0;
        break;
      case keymap_platform::x11:
        has_native = has_native || resolved.keycode != 0;
        break;
      case keymap_platform::macos:
        // KeyA is key code 0, which is a valid macOS value.
        has_native = has_native || resolved.value != 0 || code == "KeyA";
        break;
    }
    expect(has_native, "a supported key carries a native value",
           name + " " + code);
    if (platform == keymap_platform::x11 && !resolved.unsupported) {
      expect(!resolved.keysym.empty(), "X11 key has a keysym",
             name + " " + code);
    }
  }
}

void spot_checks() {
  // Numpad digits must not degrade into the main digit row.
  const key_lookup win_numpad1 =
      molink::lookup_key(keymap_platform::windows, "Numpad1");
  expect(win_numpad1.found && !win_numpad1.unsupported &&
             win_numpad1.value == 0x61 && win_numpad1.scan == 0x4F &&
             !win_numpad1.extended,
         "Windows Numpad1 is scan 4F (VK_NUMPAD1, no E0)", "0x4F");
  const key_lookup x11_numpad1 =
      molink::lookup_key(keymap_platform::x11, "Numpad1");
  expect(x11_numpad1.keysym == "KP_1" && x11_numpad1.keycode == 87,
         "X11 Numpad1 is KP_1 on key code 87", "87");
  const key_lookup mac_numpad1 =
      molink::lookup_key(keymap_platform::macos, "Numpad1");
  expect(mac_numpad1.value == 83, "macOS Numpad1 is CGKeyCode 83", "83");

  // The two extended keypad keys, plus the navigation block.
  const key_lookup numpad_enter =
      molink::lookup_key(keymap_platform::windows, "NumpadEnter");
  expect(numpad_enter.value == 0x0D && numpad_enter.scan == 0x1C &&
             numpad_enter.extended,
         "Windows NumpadEnter is extended scan 1C", "0x1C/E0");
  const key_lookup numpad_divide =
      molink::lookup_key(keymap_platform::windows, "NumpadDivide");
  expect(numpad_divide.value == 0x6F && numpad_divide.scan == 0x35 &&
             numpad_divide.extended,
         "Windows NumpadDivide is extended scan 35", "0x35/E0");
  const key_lookup numpad_equal =
      molink::lookup_key(keymap_platform::windows, "NumpadEqual");
  expect(numpad_equal.value == 0x92 && numpad_equal.scan == 0x59,
         "Windows NumpadEqual is scan 59", "0x59");
  const key_lookup numpad_add =
      molink::lookup_key(keymap_platform::x11, "NumpadAdd");
  expect(numpad_add.keysym == "KP_Add" && numpad_add.keycode == 86,
         "X11 NumpadAdd is KP_Add on key code 86", "86");

  // Function keys past F12, an international key and the media/browser block.
  const key_lookup f13 = molink::lookup_key(keymap_platform::windows, "F13");
  expect(f13.value == 0x7C && f13.scan == 0x64,
         "Windows F13 is scan 64 (VK_F13)", "0x64");
  const key_lookup intl =
      molink::lookup_key(keymap_platform::windows, "IntlBackslash");
  expect(intl.value == 0xE2 && intl.scan == 0x56,
         "Windows IntlBackslash is scan 56 (VK_OEM_102)", "0x56");
  const key_lookup volume =
      molink::lookup_key(keymap_platform::windows, "AudioVolumeUp");
  expect(volume.value == 0xAF && volume.scan == 0x30 && volume.extended,
         "Windows AudioVolumeUp is extended scan 30", "0x30/E0");
  const key_lookup browser =
      molink::lookup_key(keymap_platform::windows, "BrowserBack");
  expect(browser.value == 0xA6 && browser.scan == 0x6A && browser.extended,
         "Windows BrowserBack is extended scan 6A", "0x6A/E0");

  // Power/WakeUp have no virtual key; their E0 scan code is the only way to
  // send them.
  const key_lookup power = molink::lookup_key(keymap_platform::windows, "Power");
  expect(power.scan == 0x5E && power.value == 0 && power.extended,
         "Windows Power is the E0 5E scan code", "0x5E");
  const key_lookup wake = molink::lookup_key(keymap_platform::windows, "WakeUp");
  expect(wake.scan == 0x63 && wake.value == 0 && wake.extended,
         "Windows WakeUp is the E0 63 scan code", "0x63");

  // Codes with no physical key on an OS are explicit, not silently dropped.
  const key_lookup again = molink::lookup_key(keymap_platform::windows, "Again");
  expect(again.found && again.unsupported,
         "Windows Again is explicitly unsupported", "no VK");
  const key_lookup x11_select =
      molink::lookup_key(keymap_platform::x11, "Select");
  expect(x11_select.found && x11_select.unsupported,
         "X11 Select is explicitly unsupported", "no evdev key");
  const key_lookup mac_print =
      molink::lookup_key(keymap_platform::macos, "PrintScreen");
  expect(mac_print.found && mac_print.unsupported,
         "macOS PrintScreen is explicitly unsupported", "no Carbon key code");
  const key_lookup mac_media =
      molink::lookup_key(keymap_platform::macos, "MediaPlayPause");
  expect(mac_media.found && mac_media.unsupported,
         "macOS media transport keys are explicitly unsupported", "no key code");

  // Unknown codes stay unknown on every platform.
  for (const keymap_platform platform :
       {keymap_platform::windows, keymap_platform::x11,
        keymap_platform::macos}) {
    expect(!molink::lookup_key(platform, "BogusKey").found,
           "an unknown code is not found", platform_name(platform));
    expect(!molink::lookup_key(platform, "F25").found,
           "F25 is outside the table", platform_name(platform));
  }

  // Legacy aliases and bare characters still resolve through the same path.
  expect(molink::lookup_key(keymap_platform::windows, "OSLeft").found,
         "OSLeft reaches MetaLeft", "windows");
  expect(molink::lookup_key(keymap_platform::x11, "r").keysym == "r",
         "a bare letter still resolves", "x11");
  expect(molink::lookup_key(keymap_platform::windows, "7").value == '7',
         "a bare digit still resolves", "windows");

  // Only the keys Windows/X11 translate through NumLock need compensation.
  expect(molink::key_depends_on_numlock("Numpad1"), "Numpad1 needs NumLock",
         "Numpad1");
  expect(molink::key_depends_on_numlock("NumpadDecimal"),
         "NumpadDecimal needs NumLock", "NumpadDecimal");
  expect(!molink::key_depends_on_numlock("NumpadAdd"),
         "NumpadAdd does not need NumLock", "NumpadAdd");
  expect(!molink::key_depends_on_numlock("Digit1"),
         "the main digit row does not need NumLock", "Digit1");
}

void layout_independence() {
  // A US and a German host disagree about the character typed at these
  // positions (Y/Z swap, punctuation moves), but they agree about the
  // position. lookup_key has no layout input on purpose: these numbers are
  // the same in both cases.
  struct physical_expectation {
    const char* code;
    unsigned int scan;
    unsigned int keycode;
  };
  const physical_expectation expected[] = {
      {"KeyY", 0x15, 29},        // German: this position types 'z'
      {"KeyZ", 0x2C, 52},        // German: this position types 'y'
      {"Semicolon", 0x27, 47},   // German: Ö
      {"IntlBackslash", 0x56, 94},
      {"Backquote", 0x29, 49},
      {"Digit1", 0x02, 10},
      {"F1", 0x3B, 67},
      {"Numpad1", 0x4F, 87},
  };
  for (const physical_expectation& item : expected) {
    const key_lookup win =
        molink::lookup_key(keymap_platform::windows, item.code);
    expect(win.scan == item.scan, "Windows scan code is position-based",
           std::string(item.code) + " " + std::to_string(win.scan));
    const key_lookup x =
        molink::lookup_key(keymap_platform::x11, item.code);
    expect(x.keycode == item.keycode, "X11 key code is evdev + 8",
           std::string(item.code) + " " + std::to_string(x.keycode));
  }
}

void text_encoding_checks() {
  // Latin-1 maps to itself; everything else uses the 0x01000000 Unicode
  // keysym range. Chinese and emoji must reach the X host unchanged.
  expect(molink::x11_unicode_keysym('A') == 'A', "X11 Latin-1 keysym",
         "A");
  expect(molink::x11_unicode_keysym(0x4E2D) == 0x01004E2D,
         "X11 Chinese keysym", "0x01004E2D");
  expect(molink::x11_unicode_keysym(0x1F600) == 0x0101F600,
         "X11 emoji keysym", "0x0101F600");
  expect(molink::x11_unicode_keysym('\n') == molink::kX11ReturnKeysym,
         "X11 newline is Return", "0xff0d");
  expect(molink::x11_unicode_keysym('\t') == molink::kX11TabKeysym,
         "X11 tab is Tab", "0xff09");
  expect(molink::x11_unicode_keysym(0x7F) == 0,
         "X11 DEL has no printable keysym", "0");

  // Windows sends UTF-16 code units, with a surrogate pair for an astral
  // code point.
  const std::u16string units =
      molink::utf16_units({0x4E2D, 0x1F600, 'A'});
  expect(units.size() == 4, "UTF-16 has four units for CJK + emoji + A",
         std::to_string(units.size()));
  expect(units[0] == 0x4E2D, "CJK unit", "0x4E2D");
  expect(units[1] == 0xD83D && units[2] == 0xDE00, "emoji surrogate pair",
         "0xD83D 0xDE00");
  expect(units[3] == 'A', "ASCII unit", "A");
}

}  // namespace

int main() {
  // No duplicate entries: a copy-paste mistake would make one code silently
  // unreachable even though the coverage loop passed for the other one.
  for (std::size_t i = 0; i < molink::kCanonicalKeyCodeCount; ++i) {
    for (std::size_t j = i + 1; j < molink::kCanonicalKeyCodeCount; ++j) {
      expect(std::string(molink::kCanonicalKeyCodes[i]) !=
                 molink::kCanonicalKeyCodes[j],
             "canonical list has no duplicates",
             molink::kCanonicalKeyCodes[i]);
    }
  }

  // The Windows table is plain data, so Linux runs the same assertions the
  // windows-core job does; a gap fails on every push, not only on Windows.
  check_platform(keymap_platform::windows);
  check_platform(keymap_platform::x11);
  check_platform(keymap_platform::macos);
  spot_checks();
  layout_independence();
  text_encoding_checks();

#if defined(_WIN32)
  // The named VK expectations are already static_asserts in keymap.cpp; this
  // extra pass runs at runtime on Windows so a regression is reported with
  // the same message as the other platforms.
  expect(molink::lookup_key(keymap_platform::windows, "Numpad1").value ==
             VK_NUMPAD1,
         "Numpad1 matches VK_NUMPAD1 at runtime", "windows");
  expect(molink::lookup_key(keymap_platform::windows, "PrintScreen").value ==
             VK_SNAPSHOT,
         "PrintScreen matches VK_SNAPSHOT at runtime", "windows");
#endif

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("keymap: all %zu canonical codes resolve on 3 platforms\n",
              molink::kCanonicalKeyCodeCount);
  return 0;
}
