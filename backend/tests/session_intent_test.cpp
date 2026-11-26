// Session intent: what a client asked the connection to do. A desktop viewer
// gets the picture and input; files/chat/terminal pages must stay off both
// paths regardless of what capabilities their hello advertises.
#include <cstdio>
#include <string>

#include "session_intent.h"

namespace {

using molink::capability;
using molink::capability_bit;
using molink::normalize_session_intent;
using molink::intent_allows_input;
using molink::session_intent_allows_push;
using molink::session_intent_capabilities;
using molink::session_intent_name;
using molink::intent_wants_video;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_intent(const std::string& actual, const std::string& wanted,
                   const char* description) {
  ++checks;
  if (actual == wanted) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s (got %s, want %s)\n", description,
               actual.c_str(), wanted.c_str());
}

void test_normalize() {
  expect_intent(normalize_session_intent("desktop"), "desktop",
                "explicit desktop survives");
  expect_intent(normalize_session_intent("files"), "files", "files survives");
  expect_intent(normalize_session_intent("chat"), "chat", "chat survives");
  expect_intent(normalize_session_intent("terminal"), "terminal",
                "terminal survives");
  // An older client sends no field at all: it is a desktop viewer.
  expect_intent(normalize_session_intent(""), "desktop",
                "empty intent defaults to desktop");
  expect_intent(normalize_session_intent("FILE"), "desktop",
                "the field is matched case-insensitively");
  expect_intent(normalize_session_intent("browser"), "desktop",
                "an unknown purpose is a desktop session");
}

void test_video_and_input() {
  expect(intent_wants_video("desktop"), "desktop wants video");
  expect(intent_wants_video(""), "a missing intent wants video");
  expect(!intent_wants_video("files"), "files does not want video");
  expect(!intent_wants_video("chat"), "chat does not want video");
  expect(!intent_wants_video("terminal"), "terminal does not want video");

  expect(intent_allows_input("desktop"), "desktop may inject input");
  expect(intent_allows_input(""), "a missing intent may inject input");
  expect(!intent_allows_input("files"), "files may not inject input");
  expect(!intent_allows_input("chat"), "chat may not inject input");
  expect(!intent_allows_input("terminal"), "terminal may not inject input");

  expect(session_intent_allows_push("desktop"), "desktop may subscribe to push");
  expect(!session_intent_allows_push("files"), "files never subscribes to push");
  expect(!session_intent_allows_push("chat"), "chat never subscribes to push");
  expect(!session_intent_allows_push("terminal"),
         "terminal never subscribes to push");
}

void test_capabilities() {
  const uint32_t all = 0xffffffffu;
  expect(session_intent_capabilities(all, "desktop") == all,
         "desktop keeps every capability");
  const uint32_t files = session_intent_capabilities(all, "files");
  expect((files & capability_bit(capability::screen)) == 0,
         "a files page loses the screen bit");
  expect((files & capability_bit(capability::input)) == 0,
         "a files page loses the input bit");
  expect((files & capability_bit(capability::files_read)) != 0,
         "a files page keeps its files bit");
  expect((files & capability_bit(capability::chat)) != 0,
         "a files page keeps the chat bit");
}

void test_names() {
  expect(std::string(session_intent_name("files")) == "files", "files name");
  expect(std::string(session_intent_name("chat")) == "chat", "chat name");
  expect(std::string(session_intent_name("terminal")) == "terminal",
         "terminal name");
  expect(std::string(session_intent_name("")) == "desktop",
         "unknown name is desktop");
}

}  // namespace

int main() {
  test_normalize();
  test_video_and_input();
  test_capabilities();
  test_names();
  if (failures != 0) {
    std::fprintf(stderr, "%d/%d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("session intent: %d checks passed\n", checks);
  return 0;
}
