// Welcome monitor parsing and the switch/reset decision, without Win32.
#include <cstdio>
#include <string>

#include "display_selector.h"

namespace {

using namespace molink::display;

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

const char* kWelcome =
    R"({"type":"welcome","display_index":1,"displays":[)"
    R"({"index":0,"name":"\\\\.\\DISPLAY1","x":0,"y":0,"width":1920,"height":1080,"primary":true},)"
    R"({"index":1,"name":"副屏","x":1920,"y":0,"width":2560,"height":1440,"primary":false}]})";

void test_parse_displays() {
  const welcome state = parse_welcome(kWelcome);
  expect(state.list.size() == 2, "two displays parsed");
  expect(state.has_current, "display_index is recorded");
  expect(state.current == 1, "the current display is the second one");
  if (state.list.size() == 2) {
    expect(state.list[0].index == 0, "first display index");
    expect_eq(state.list[0].name, "\\\\.\\DISPLAY1",
              "escaped monitor name is unescaped");
    expect(state.list[0].width == 1920 && state.list[0].height == 1080,
           "first display size");
    expect(state.list[0].primary, "first display is primary");
    expect(state.list[1].index == 1, "second display index");
    expect_eq(state.list[1].name, "副屏", "\\u escapes decode to UTF-8");
    expect(state.list[1].x == 1920, "second display origin");
    expect(!state.list[1].primary, "second display is not primary");
  }
}

void test_missing_index_falls_back_to_position() {
  const welcome state = parse_welcome(
      R"({"displays":[{"name":"A","width":800,"height":600},)"
      R"({"name":"B","width":1024,"height":768}]})");
  expect(state.list.size() == 2, "both displays parsed without index");
  if (state.list.size() == 2) {
    expect(state.list[0].index == 0 && state.list[1].index == 1,
           "array position supplies the index");
  }
  expect(!state.has_current, "no display_index means no explicit current");
}

void test_empty_and_malformed_welcome() {
  const welcome empty = parse_welcome(R"({"type":"welcome"})");
  expect(empty.list.empty(), "no displays key yields an empty list");
  expect(empty.current == 0, "an empty welcome starts at display 0");
  const welcome broken = parse_welcome(R"({"displays":[{"index":0,)");
  expect(broken.list.empty(), "a truncated object is ignored");
}

void test_switch_plan() {
  const welcome state = parse_welcome(kWelcome);
  const switch_plan same = plan_switch(state, 1, 1);
  expect(!same.changed && !same.reset_picture,
         "selecting the current display changes nothing");
  const switch_plan real = plan_switch(state, 1, 0);
  expect(real.changed, "another display is a change");
  expect(real.reset_picture, "another display resets the decoder");
  expect(real.index == 0, "the plan carries the new index");
  const switch_plan unknown = plan_switch(state, 1, 9);
  expect(!unknown.changed && !unknown.reset_picture,
         "an unknown display index is ignored");
  expect(unknown.index == 1, "an ignored switch keeps the current index");

  const welcome legacy = parse_welcome(R"({"type":"welcome"})");
  const switch_plan any = plan_switch(legacy, 0, 2);
  expect(any.changed && any.reset_picture,
         "an older host without a list still accepts the request");
}

}  // namespace

int main() {
  test_parse_displays();
  test_missing_index_falls_back_to_position();
  test_empty_and_malformed_welcome();
  test_switch_plan();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
