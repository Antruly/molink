// Coverage test for the host keyboard tables.
//
// The same table is used by every platform injector, so it is checked here
// without a display, a window or an OS keyboard API. Every canonical
// KeyboardEvent.code must resolve to a native key or be explicitly marked
// unsupported; a missing entry is the "the keypad does nothing" bug this test
// exists to prevent.
#include <cstdio>
#include <initializer_list>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "input/keymap.h"

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
    expect(resolved.found && (resolved.unsupported || resolved.value != 0 ||
                              resolved.scan_code ||
                              (platform == keymap_platform::x11) ||
                              (platform == keymap_platform::macos &&
                               code == "KeyA")),
           "a supported key carries a native value",
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
             win_numpad1.value == 0x61 && !win_numpad1.extended,
         "Windows Numpad1 is VK_NUMPAD1 without the E0 prefix", "0x61");
  const key_lookup x11_numpad1 =
      molink::lookup_key(keymap_platform::x11, "Numpad1");
  expect(x11_numpad1.keysym == "KP_1", "X11 Numpad1 is KP_1",
         x11_numpad1.keysym);
  const key_lookup mac_numpad1 =
      molink::lookup_key(keymap_platform::macos, "Numpad1");
  expect(mac_numpad1.value == 83, "macOS Numpad1 is CGKeyCode 83", "83");

  // The two extended keypad keys, plus the navigation block.
  const key_lookup numpad_enter =
      molink::lookup_key(keymap_platform::windows, "NumpadEnter");
  expect(numpad_enter.value == 0x0D && numpad_enter.extended,
         "Windows NumpadEnter is extended VK_RETURN", "0x0D/E0");
  const key_lookup numpad_divide =
      molink::lookup_key(keymap_platform::windows, "NumpadDivide");
  expect(numpad_divide.value == 0x6F && numpad_divide.extended,
         "Windows NumpadDivide is extended VK_DIVIDE", "0x6F/E0");
  const key_lookup numpad_equal =
      molink::lookup_key(keymap_platform::windows, "NumpadEqual");
  expect(numpad_equal.value == 0x92 && !numpad_equal.extended,
         "Windows NumpadEqual is VK_OEM_NEC_EQUAL", "0x92");
  const key_lookup numpad_add =
      molink::lookup_key(keymap_platform::x11, "NumpadAdd");
  expect(numpad_add.keysym == "KP_Add", "X11 NumpadAdd is KP_Add",
         numpad_add.keysym);

  // Function keys past F12, an international key and the media/browser block.
  const key_lookup f13 = molink::lookup_key(keymap_platform::windows, "F13");
  expect(f13.value == 0x7C, "Windows F13 is VK_F13", "0x7C");
  const key_lookup intl =
      molink::lookup_key(keymap_platform::windows, "IntlBackslash");
  expect(intl.value == 0xE2, "Windows IntlBackslash is VK_OEM_102", "0xE2");
  const key_lookup volume =
      molink::lookup_key(keymap_platform::windows, "AudioVolumeUp");
  expect(volume.value == 0xAF && volume.extended,
         "Windows AudioVolumeUp is extended VK_VOLUME_UP", "0xAF");
  const key_lookup browser =
      molink::lookup_key(keymap_platform::windows, "BrowserBack");
  expect(browser.value == 0xA6 && browser.extended,
         "Windows BrowserBack is extended VK_BROWSER_BACK", "0xA6");

  // Power/WakeUp have no virtual key; they must be sent as E0 scan codes.
  const key_lookup power = molink::lookup_key(keymap_platform::windows, "Power");
  expect(power.scan_code && power.value == 0x5E && power.extended,
         "Windows Power is the E0 5E scan code", "0x5E");
  const key_lookup wake = molink::lookup_key(keymap_platform::windows, "WakeUp");
  expect(wake.scan_code && wake.value == 0x63 && wake.extended,
         "Windows WakeUp is the E0 63 scan code", "0x63");

  // Codes with no physical key on an OS are explicit, not silently dropped.
  const key_lookup again = molink::lookup_key(keymap_platform::windows, "Again");
  expect(again.found && again.unsupported,
         "Windows Again is explicitly unsupported", "no VK");
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
