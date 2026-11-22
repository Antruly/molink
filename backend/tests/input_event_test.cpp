#include <iostream>
#include <string>

#include "input/input_event.h"
#include "input/key_alias.h"

namespace {

using molink::input_event;
using molink::input_injector;

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
    return button >= 1 && button <= 3 ? status::ok : status::rejected;
  }
  status wheel(int dx, int dy) override {
    (void)dx;
    (void)dy;
    return status::ok;
  }
  status key(const std::string& code, bool down) override {
    (void)down;
    // The host backends canonicalise first; the injector only sees the final
    // spelling. Anything else is an unknown key.
    return code == "MetaLeft" || code == "MetaRight" || code == "KeyR"
               ? status::ok
               : status::rejected;
  }
  status text(const std::string&) override { return status::ok; }
  void release_all() override {}
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

input_event key_event(const std::string& code) {
  input_event event;
  event.type = input_event::kind::key;
  event.code = code;
  return event;
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
  std::string err;
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
