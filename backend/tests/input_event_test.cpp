#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "input/display_rect.h"
#include "input/input_event.h"
#include "input/key_alias.h"

namespace {

using molink::display_rect_state;
using molink::held_key_state;
using molink::input_event;
using molink::input_injector;
using molink::wheel_accumulator;

class fake_input final : public input_injector {
 public:
  bool available() const override { return true; }
  std::string backend() const override { return "fake"; }
  status move(double nx, double ny) override {
    (void)nx;
    (void)ny;
    return status::ok;
  }
  status button(int button, bool down) override {
    (void)down;
    return button >= 1 && button <= 5 ? status::ok : status::rejected;
  }
  status wheel(int dx, int dy) override {
    wheel_calls.push_back({dx, dy});
    return status::ok;
  }
  status key(const std::string& code, bool down) override {
    key_calls.push_back({code, down});
    // The host backends canonicalise first; the injector only sees the final
    // spelling. Anything else is an unknown key.
    return code == "MetaLeft" || code == "MetaRight" || code == "KeyR" ||
                   code == "KeyA" || code == "ControlLeft" || code == "ShiftLeft"
               ? status::ok
               : status::rejected;
  }
  status text(const std::string&) override { return status::ok; }
  void release_all() override {}

  std::vector<std::pair<int, int>> wheel_calls;
  std::vector<std::pair<std::string, bool>> key_calls;
};

int failures = 0;

void expect(bool condition, const char* what) {
  if (condition) return;
  std::cerr << "FAIL: " << what << "\n";
  ++failures;
}

void expect_rejected(input_injector& input, const input_event& event,
                     const std::string& expected) {
  std::string err;
  const input_injector::status status =
      molink::inject_input_event(input, event, err);
  expect(status == input_injector::status::rejected, "status is rejected");
  expect(!err.empty(), "rejected input carries a message");
  expect(err == expected, expected.c_str());
}

input_event key_event(const std::string& code, bool down = true) {
  input_event event;
  event.type = input_event::kind::key;
  event.code = code;
  event.down = down;
  return event;
}

input_event wheel_event(double dx, double dy) {
  input_event event;
  event.type = input_event::kind::wheel;
  event.dx = dx;
  event.dy = dy;
  return event;
}

void expect_wheel(const fake_input& input, std::size_t index, int dx, int dy,
                  const char* what) {
  expect(input.wheel_calls.size() > index, what);
  if (input.wheel_calls.size() <= index) return;
  expect(input.wheel_calls[index].first == dx &&
             input.wheel_calls[index].second == dy,
         what);
}

void test_wheel_rounding_without_accumulator() {
  fake_input input;
  std::string err;
  // A fractional notch used to be truncated to zero. It is rounded now: a
  // value below half a notch still emits 0, one above half emits a notch.
  expect(molink::inject_input_event(input, wheel_event(0.0, 0.4), err) ==
             input_injector::status::ok,
         "small wheel delta is accepted");
  expect_wheel(input, 0, 0, 0, "0.4 of a notch rounds to 0");
  expect(molink::inject_input_event(input, wheel_event(0.0, 0.6), err) ==
             input_injector::status::ok,
         "second wheel delta is accepted");
  expect_wheel(input, 1, 0, 1, "0.6 of a notch rounds to 1");
  expect(molink::inject_input_event(input, wheel_event(0.6, -0.6), err) ==
             input_injector::status::ok,
         "horizontal wheel delta is accepted");
  expect_wheel(input, 2, 1, -1, "dx and dy round independently");
}

void test_wheel_fractions_accumulate() {
  fake_input input;
  wheel_accumulator wheel;
  std::string err;
  // Three 40 px events are one 120 px notch. The first two carry the
  // fraction, the third (or the one that crosses half a notch) fires.
  for (int i = 0; i < 2; ++i) {
    expect(molink::inject_input_event(input, wheel_event(0.0, 0.4), err,
                                      nullptr, &wheel) ==
               input_injector::status::ok,
           "fractional wheel event is accepted");
  }
  expect_wheel(input, 0, 0, 0, "0.4 stays a fraction");
  expect_wheel(input, 1, 0, 1, "0.8 rounds to one notch and keeps -0.2");
  expect(molink::inject_input_event(input, wheel_event(0.0, 0.4), err, nullptr,
                                    &wheel) ==
             input_injector::status::ok,
         "third fractional wheel event is accepted");
  expect_wheel(input, 2, 0, 0, "the carried -0.2 leaves no whole notch yet");
  expect(molink::inject_input_event(input, wheel_event(0.0, 0.4), err, nullptr,
                                    &wheel) ==
             input_injector::status::ok,
         "fourth fractional wheel event is accepted");
  expect_wheel(input, 3, 0, 1, "0.6 of a notch fires and carries -0.4");
  expect(molink::inject_input_event(input, wheel_event(0.0, 0.4), err, nullptr,
                                    &wheel) ==
             input_injector::status::ok,
         "fifth fractional wheel event is accepted");
  expect_wheel(input, 4, 0, 0, "the carried -0.4 plus 0.4 is no notch yet");

  // Negative deltas accumulate the same way.
  wheel.reset();
  input.wheel_calls.clear();
  for (int i = 0; i < 2; ++i) {
    molink::inject_input_event(input, wheel_event(0.0, -0.4), err, nullptr,
                               &wheel);
  }
  expect_wheel(input, 1, 0, -1, "negative fractions accumulate too");
}

void test_wheel_accumulator_is_bounded() {
  wheel_accumulator wheel;
  const wheel_accumulator::notches first = wheel.add(100.0, -100.0);
  expect(first.dx == 10 && first.dy == -10,
         "a hostile delta is clamped to the injector's limit");
  const wheel_accumulator::notches second = wheel.add(0.0, 0.0);
  expect(second.dx == 0 && second.dy == 0,
         "the clamp does not leave a debt that keeps scrolling");
}

void test_held_keys_release_only_own_codes() {
  held_key_state keys;
  keys.note("ControlLeft", true, 1000);
  keys.note("KeyA", true, 1000);
  expect(keys.held("ControlLeft"), "a key-down is recorded");
  expect(keys.size() == 2, "both keys are held");
  keys.note("KeyA", false, 1001);
  expect(!keys.held("KeyA") && keys.size() == 1,
         "a key-up removes only that code");
  // A duplicate key-down (auto-repeat) does not duplicate the entry.
  keys.note("ControlLeft", true, 1002);
  expect(keys.size() == 1, "a duplicate key-down keeps one entry");

  const std::vector<input_event> released = keys.release_all();
  expect(released.size() == 1, "release_all emits one key-up");
  if (!released.empty()) {
    expect(released[0].type == input_event::kind::key, "release is a key event");
    expect(released[0].code == "ControlLeft", "release names the held code");
    expect(!released[0].down, "release is a key-up");
  }
  expect(keys.size() == 0, "release_all forgets the held keys");
  expect(keys.release_all().empty(), "a second release_all is empty");
}

void test_held_keys_expire_modifiers_only() {
  held_key_state keys;
  keys.note("ControlLeft", true, 1000);
  keys.note("ShiftLeft", true, 2000);
  keys.note("KeyA", true, 3000);
  constexpr std::uint64_t kTimeout = 360000;

  expect(keys.expire(1000 + kTimeout - 1, kTimeout).empty(),
         "nothing expires one millisecond early");
  auto expired = keys.expire(1000 + kTimeout, kTimeout);
  expect(expired.size() == 1, "the oldest modifier expires");
  if (!expired.empty()) {
    expect(expired[0].code == "ControlLeft" && !expired[0].down,
           "the expired modifier is released");
  }
  expect(keys.held("ShiftLeft"), "a newer modifier stays held");
  expect(keys.held("KeyA"), "a non-modifier is never expired by the scan");

  expired = keys.expire(2000 + kTimeout, kTimeout);
  expect(expired.size() == 1 && expired[0].code == "ShiftLeft",
         "the second modifier expires on its own lease");
  expect(keys.size() == 1, "only the non-modifier remains");
}

void test_held_key_modifier_table() {
  const char* const modifiers[] = {"ShiftLeft", "ShiftRight", "ControlLeft",
                                   "ControlRight", "AltLeft", "AltRight",
                                   "MetaLeft", "MetaRight", "AltGraph"};
  for (const char* code : modifiers) {
    expect(held_key_state::is_modifier(code), code);
  }
  expect(!held_key_state::is_modifier("KeyA"), "letters are not modifiers");
  expect(!held_key_state::is_modifier("CapsLock"),
         "CapsLock is a lock, not a held modifier");
}

void test_display_rect_updates_only_on_change() {
  display_rect_state rect;
  expect(!rect.has_rect(), "no rectangle before the first report");
  expect(rect.update(0, 0, 1920, 1080), "the first rectangle is an update");
  expect(rect.has_rect(), "the first rectangle is remembered");
  expect(!rect.update(0, 0, 1920, 1080), "the same rectangle is not an update");
  expect(rect.update(0, 0, 2560, 1440), "a resolution change is an update");
  expect(rect.current().width == 2560 && rect.current().height == 1440,
         "the new size is remembered");
  expect(rect.update(1920, 0, 1280, 1024),
         "a display switch (origin and size) is an update");
  expect(!rect.update(0, 0, 0, 1080),
         "an empty capture is ignored instead of zeroing the rectangle");
  expect(rect.current().width == 1280,
         "an ignored report keeps the last good rectangle");
}

}  // namespace

int main() {
  fake_input input;

  // An unknown code names the key, not just "bad_input".
  expect_rejected(input, key_event("BogusKey"), "unknown key code: BogusKey");

  // Missing code is a validation error with its own message.
  expect_rejected(input, key_event(""), "key needs a code");

  // Rejections from the platform layer get a non-empty fallback too.
  input_event bad_button;
  bad_button.type = input_event::kind::button;
  bad_button.has_button = true;
  bad_button.button = 99;
  expect_rejected(input, bad_button, "unknown mouse button: 99");

  input_event empty_text;
  empty_text.type = input_event::kind::text;
  expect_rejected(input, empty_text, "text must be 1..4096 bytes");

  input_event unknown_kind;
  unknown_kind.type = input_event::kind::unknown;
  unknown_kind.kind_name = "teleport";
  expect_rejected(input, unknown_kind, "unknown input event kind: teleport");

  // Side buttons map through the same validation: 4/5 are accepted now.
  input_event back_button;
  back_button.type = input_event::kind::button;
  back_button.has_button = true;
  back_button.button = 4;
  std::string err;
  expect(molink::inject_input_event(input, back_button, err) ==
             input_injector::status::ok,
         "button 4 (back) is accepted");
  back_button.button = 5;
  expect(molink::inject_input_event(input, back_button, err) ==
             input_injector::status::ok,
         "button 5 (forward) is accepted");

  test_wheel_rounding_without_accumulator();
  test_wheel_fractions_accumulate();
  test_wheel_accumulator_is_bounded();
  test_held_keys_release_only_own_codes();
  test_held_keys_expire_modifiers_only();
  test_held_key_modifier_table();
  test_display_rect_updates_only_on_change();

  // The alias table the old clients rely on.
  expect(molink::canonical_key_code("OSLeft") == "MetaLeft", "OSLeft alias");
  expect(molink::canonical_key_code("OSRight") == "MetaRight", "OSRight alias");
  expect(molink::canonical_key_code("Win") == "MetaLeft", "Win alias");
  expect(molink::canonical_key_code("Super") == "MetaLeft", "Super alias");
  expect(molink::canonical_key_code("超") == "MetaLeft", "CJK alias");
  expect(molink::canonical_key_code("r") == "KeyR", "lowercase letter alias");
  expect(molink::canonical_key_code("7") == "Digit7", "digit alias");
  expect(molink::canonical_key_code("KeyR") == "KeyR", "canonical passes");
  expect(molink::canonical_key_code("Launch5") == "Launch5", "unknown passes");

  input_event meta = key_event(molink::canonical_key_code("OSLeft"));
  expect(molink::inject_input_event(input, meta, err) ==
             input_injector::status::ok,
         "canonical MetaLeft is accepted");
  expect(err.empty(), "accepted input leaves the message empty");

  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  std::cout << "input_event: all checks passed\n";
  return 0;
}
