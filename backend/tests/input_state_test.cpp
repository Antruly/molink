// Keyboard translation and pressed-key state, driven without Win32 so it
// runs on Linux next to the other protocol tests. The Windows shell compiles
// the same input_state.cpp.
#include <cstdio>
#include <string>
#include <vector>

#include "input_state.h"

namespace {

using namespace molink::input;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_eq(const std::string& actual, const std::string& expected,
               const char* description) {
  ++checks;
  if (actual == expected) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n",
               description, expected.c_str(), actual.c_str());
}

void expect_events(const std::vector<key_event>& events,
                   const std::vector<std::pair<std::string, bool>>& expected,
                   const char* description) {
  ++checks;
  if (events.size() != expected.size()) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s (expected %zu events, got %zu)\n",
                 description, expected.size(), events.size());
    return;
  }
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (events[i].code == expected[i].first &&
        events[i].down == expected[i].second) {
      continue;
    }
    ++failures;
    std::fprintf(stderr,
                 "FAIL: %s at event %zu: expected %s %s, got %s %s\n",
                 description, i, expected[i].first.c_str(),
                 expected[i].second ? "down" : "up", events[i].code.c_str(),
                 events[i].down ? "down" : "up");
    return;
  }
}

void expect_code(int vk, bool extended, bool numlock_on, const char* expected,
                 const char* description) {
  expect_eq(key_code(vk, extended, numlock_on), expected, description);
}

void test_repeat_keydown_is_dropped() {
  key_state state;
  expect_events(state.key('A', false, true, false, true),
                {{"KeyA", true}}, "the first key-down is sent");
  expect_events(state.key('A', false, true, true, true), {},
                "an auto-repeat key-down is dropped");
  expect_events(state.key('A', false, true, false, true), {},
                "a duplicate non-repeat key-down is dropped");
  expect(state.held_count() == 1, "the key stays held exactly once");
  expect_events(state.key('A', false, true, false, false),
                {{"KeyA", false}}, "the key-up is sent");
  expect(state.held_count() == 0, "the key-up clears the held state");
}

void test_unmapped_key_is_dropped() {
  key_state state;
  expect_events(state.key(0xFE, false, true, false, true), {},
                "an unmapped virtual key sends nothing");
  expect(state.held_count() == 0, "an unmapped key is not tracked");
}

void test_unrecorded_key_up_is_still_sent() {
  key_state state;
  expect_events(state.key('C', false, true, false, false),
                {{"KeyC", false}},
                "a key-up for an untracked code still releases the host");
  expect(state.held_count() == 0, "an untracked key-up records nothing");
}

void test_release_all_releases_every_held_key() {
  key_state state;
  state.key('A', false, true, false, true);
  state.key(vk::K_LSHIFT, false, true, false, true);
  state.key('C', false, true, false, true);
  expect(state.held_count() == 3, "three keys are held");
  // Newest first: C, Shift, A.
  expect_events(state.release_all(),
                {{"KeyC", false}, {"ShiftLeft", false}, {"KeyA", false}},
                "release_all sends a key-up for every held key");
  expect(state.held_count() == 0, "release_all forgets the held keys");
  expect_events(state.release_all(), {},
                "a second release_all has nothing to send");

  // A release must remove exactly the keys it emitted, not the whole set by
  // accident: press again and forget without events.
  state.key('D', false, true, false, true);
  state.forget();
  expect(state.held_count() == 0, "forget drops the held state silently");
  expect_events(state.release_all(), {},
                "forgotten keys are not released twice");
}

void test_numpad_is_physical_regardless_of_numlock() {
  struct pair {
    int numlock_on_vk;
    int numlock_off_vk;
    const char* expected;
  };
  const pair digits[] = {
      {vk::K_NUMPAD0, vk::K_INSERT, "Numpad0"},
      {vk::K_NUMPAD0 + 1, vk::K_END, "Numpad1"},
      {vk::K_NUMPAD0 + 2, vk::K_DOWN, "Numpad2"},
      {vk::K_NUMPAD0 + 3, vk::K_NEXT, "Numpad3"},
      {vk::K_NUMPAD0 + 4, vk::K_LEFT, "Numpad4"},
      {vk::K_NUMPAD0 + 5, vk::K_CLEAR, "Numpad5"},
      {vk::K_NUMPAD0 + 6, vk::K_RIGHT, "Numpad6"},
      {vk::K_NUMPAD0 + 7, vk::K_HOME, "Numpad7"},
      {vk::K_NUMPAD0 + 8, vk::K_UP, "Numpad8"},
      {vk::K_NUMPAD0 + 9, vk::K_PRIOR, "Numpad9"},
      {vk::K_DECIMAL, vk::K_DELETE, "NumpadDecimal"},
  };
  for (const pair& item : digits) {
    expect_code(item.numlock_on_vk, false, true, item.expected,
                "NumLock on sends the keypad code");
    expect_code(item.numlock_off_vk, false, false, item.expected,
                "NumLock off still sends the same keypad code");
  }

  // Operators do not change with NumLock; Divide, Return and Enter carry the
  // extended bit from the keypad.
  expect_code(vk::K_MULTIPLY, false, true, "NumpadMultiply", "keypad *");
  expect_code(vk::K_MULTIPLY, false, false, "NumpadMultiply", "keypad *");
  expect_code(vk::K_ADD, false, true, "NumpadAdd", "keypad +");
  expect_code(vk::K_SUBTRACT, false, true, "NumpadSubtract", "keypad -");
  expect_code(vk::K_DIVIDE, true, true, "NumpadDivide", "keypad /");
  expect_code(vk::K_DIVIDE, true, false, "NumpadDivide", "keypad /");
  expect_code(vk::K_RETURN, true, true, "NumpadEnter", "keypad Enter");
  expect_code(vk::K_RETURN, true, false, "NumpadEnter", "keypad Enter");
  expect_code(vk::K_RETURN, false, true, "Enter", "main Enter");
  expect_code(vk::K_RETURN, false, false, "Enter", "main Enter");
  expect_code(vk::K_SEPARATOR, false, true, "NumpadComma", "keypad comma");
  expect_code(vk::K_OEM_NEC_EQUAL, false, true, "NumpadEqual",
              "keypad equal");

  // The real navigation/editing cluster keeps its own codes: those keys are
  // extended, which is exactly what distinguishes them from the keypad.
  expect_code(vk::K_HOME, true, true, "Home", "navigation Home");
  expect_code(vk::K_DELETE, true, true, "Delete", "navigation Delete");
  expect_code(vk::K_INSERT, true, true, "Insert", "navigation Insert");
  expect_code(vk::K_LEFT, true, false, "ArrowLeft", "navigation Left");
  expect_code(vk::K_CLEAR, true, true, "Clear", "the standalone Clear key");
}

void test_named_keys_match_the_web_table() {
  expect_code(vk::K_APPS, true, true, "ContextMenu", "ContextMenu");
  expect_code(vk::K_HELP, false, true, "Help", "Help");
  expect_code(vk::K_SELECT, false, true, "Select", "Select");
  expect_code(vk::K_EXECUTE, false, true, "Execute", "Execute");
  expect_code(vk::K_CANCEL, false, true, "Cancel", "Cancel");
  expect_code(vk::K_SLEEP, true, true, "Sleep", "Sleep");
  expect_code(vk::K_BROWSER_BACK, true, true, "BrowserBack", "BrowserBack");
  expect_code(vk::K_BROWSER_HOME, true, true, "BrowserHome", "BrowserHome");
  expect_code(vk::K_VOLUME_MUTE, true, true, "AudioVolumeMute",
              "AudioVolumeMute");
  expect_code(vk::K_VOLUME_UP, true, true, "AudioVolumeUp", "AudioVolumeUp");
  expect_code(vk::K_MEDIA_PLAY_PAUSE, true, true, "MediaPlayPause",
              "MediaPlayPause");
  expect_code(vk::K_MEDIA_PREV_TRACK, true, true, "MediaTrackPrevious",
              "MediaTrackPrevious");
  expect_code(vk::K_LAUNCH_MAIL, true, true, "LaunchMail", "LaunchMail");
  expect_code(vk::K_LAUNCH_APP2, true, true, "LaunchApp2", "LaunchApp2");
  expect_code(vk::K_CONVERT, false, true, "Convert", "IME Convert");
  expect_code(vk::K_NONCONVERT, false, true, "NonConvert", "IME NonConvert");
  expect_code(vk::K_KANA, false, true, "KanaMode", "IME Kana/Hangul");
  expect_code(vk::K_HANJA, false, true, "HanjaMode", "IME Hanja/Kanji");
  expect_code(vk::K_OEM_102, false, true, "IntlBackslash", "IntlBackslash");
  expect_code(vk::K_OEM_3, false, true, "Backquote", "Backquote");

  // Generic modifier messages use the extended bit for the right-hand side.
  expect_code(vk::K_CONTROL, false, true, "ControlLeft", "left Control");
  expect_code(vk::K_CONTROL, true, true, "ControlRight", "right Control");
  expect_code(vk::K_MENU, false, true, "AltLeft", "left Alt");
  expect_code(vk::K_MENU, true, true, "AltRight", "right Alt");
  expect_code(vk::K_LWIN, true, true, "MetaLeft", "left Win");
  expect_code(vk::K_RWIN, true, true, "MetaRight", "right Win");
}

void test_combo_entries_match_the_web_order() {
  expect(combo_count() == 10, "the native menu has the ten Web shortcuts");
  const char* expected_ids[] = {"win-r",    "win-e",  "win-d",
                                "win-tab",  "alt-tab", "alt-f4",
                                "ctrl-esc", "ctrl-shift-esc",
                                "win-shift-s", "print-screen"};
  for (std::size_t i = 0; i < combo_count(); ++i) {
    expect_eq(combos()[i].id, expected_ids[i], "combo id and order");
  }
  expect(find_combo("ctrl-shift-esc") != nullptr, "combo lookup by id");
  expect(find_combo("win-l") == nullptr,
         "Win+L stays out of the native menu (no confirm dialog)");
}

void test_combo_injection_order() {
  key_state state;
  const key_combo* combo = find_combo("ctrl-shift-esc");
  expect(combo != nullptr, "Ctrl+Shift+Esc exists");
  if (combo == nullptr) return;
  expect_events(state.combo(*combo),
                {{"ControlLeft", true},
                 {"ShiftLeft", true},
                 {"Escape", true},
                 {"Escape", false},
                 {"ShiftLeft", false},
                 {"ControlLeft", false}},
                "modifiers down, main key down/up, modifiers reversed");
  expect(state.held_count() == 0, "a combo leaves no key held");

  const key_combo* win_r = find_combo("win-r");
  expect(win_r != nullptr, "Win+R exists");
  if (win_r == nullptr) return;
  expect_events(state.combo(*win_r),
                {{"MetaLeft", true},
                 {"KeyR", true},
                 {"KeyR", false},
                 {"MetaLeft", false}},
                "Win+R is MetaLeft down, R down/up, MetaLeft up");
  expect(state.held_count() == 0, "Win+R leaves no key held");

  // The single-key shortcut is down then up, and releasing afterwards does
  // not emit a second key-up for it.
  const key_combo* print = find_combo("print-screen");
  expect(print != nullptr, "PrintScreen exists");
  if (print == nullptr) return;
  expect_events(state.combo(*print),
                {{"PrintScreen", true}, {"PrintScreen", false}},
                "PrintScreen is one down/up pair");
  expect_events(state.release_all(), {},
                "a finished combo is not released twice");
}

}  // namespace

int main() {
  test_repeat_keydown_is_dropped();
  test_unmapped_key_is_dropped();
  test_unrecorded_key_up_is_still_sent();
  test_release_all_releases_every_held_key();
  test_numpad_is_physical_regardless_of_numlock();
  test_named_keys_match_the_web_table();
  test_combo_entries_match_the_web_order();
  test_combo_injection_order();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
