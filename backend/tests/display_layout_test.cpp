// Display hotplug/resize/rotate detection, without Win32 or an X server.
#include <cstdio>
#include <string>
#include <vector>

#include "capture/display_layout.h"

namespace {

using namespace molink;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

display_layout_entry entry(const std::string& name, int x, int y, int width,
                           int height, bool primary) {
  display_layout_entry item;
  item.name = name;
  item.x = x;
  item.y = y;
  item.width = width;
  item.height = height;
  item.primary = primary;
  return item;
}

std::vector<display_layout_entry> two_monitors() {
  return {entry("\\\\.\\DISPLAY1", 0, 0, 1920, 1080, true),
          entry("\\\\.\\DISPLAY2", 1920, 0, 2560, 1440, false)};
}

void test_identical_layout_does_not_trigger() {
  const std::vector<display_layout_entry> before = two_monitors();
  const std::vector<display_layout_entry> after = two_monitors();
  expect(compare_display_layouts(before, after) ==
             display_change_reason::none,
         "an identical list is not a change");
  expect(compare_display_layouts({}, {}) == display_change_reason::none,
         "two empty lists are not a change");
}

void test_count_change_is_hotplug() {
  std::vector<display_layout_entry> after = two_monitors();
  after.pop_back();
  expect(compare_display_layouts(two_monitors(), after) ==
             display_change_reason::hotplug,
         "unplugging a monitor is a hotplug");
  expect(compare_display_layouts(after, two_monitors()) ==
             display_change_reason::hotplug,
         "plugging a monitor in is a hotplug");
}

void test_name_change_is_hotplug() {
  std::vector<display_layout_entry> after = two_monitors();
  after[1].name = "\\\\.\\DISPLAY3";
  expect(compare_display_layouts(two_monitors(), after) ==
             display_change_reason::hotplug,
         "a different monitor at the same position is a hotplug");
}

void test_size_change_is_resize() {
  std::vector<display_layout_entry> after = two_monitors();
  after[0].width = 2560;
  expect(compare_display_layouts(two_monitors(), after) ==
             display_change_reason::resize,
         "a wider desktop is a resize");
  after = two_monitors();
  after[1].height = 1080;
  expect(compare_display_layouts(two_monitors(), after) ==
             display_change_reason::resize,
         "a shorter desktop is a resize");
}

void test_rotation_change_is_rotate() {
  std::vector<display_layout_entry> after = two_monitors();
  after[1].rotation = 2;  // DXGI_MODE_ROTATION_ROTATE90
  expect(compare_display_layouts(two_monitors(), after) ==
             display_change_reason::rotate,
         "rotation alone is a rotate change");
  // A rotated monitor usually also swaps its reported desktop width/height;
  // the more specific cause must win.
  after[1].width = 1440;
  after[1].height = 2560;
  expect(compare_display_layouts(two_monitors(), after) ==
             display_change_reason::rotate,
         "rotation that changed the size still reports rotate");
}

void test_position_and_primary_are_hotplug() {
  std::vector<display_layout_entry> after = two_monitors();
  after[1].x = 100;
  expect(compare_display_layouts(two_monitors(), after) ==
             display_change_reason::hotplug,
         "rearranging monitors is a hotplug");
  after = two_monitors();
  after[0].primary = false;
  after[1].primary = true;
  expect(compare_display_layouts(two_monitors(), after) ==
             display_change_reason::hotplug,
         "switching the primary monitor is a hotplug");
}

void test_primary_selection() {
  const std::vector<display_layout_entry> list = two_monitors();
  expect(primary_display_index(list) == 0, "the flagged first monitor wins");
  std::vector<display_layout_entry> moved = two_monitors();
  moved[0].primary = false;
  moved[1].primary = true;
  expect(primary_display_index(moved) == 1,
         "the primary flag, not the position, selects the monitor");
  std::vector<display_layout_entry> none = two_monitors();
  none[0].primary = false;
  none[1].primary = false;
  expect(primary_display_index(none) == 0,
         "without a primary flag the first monitor is the fallback");
  expect(primary_display_index({}) == 0, "an empty list yields index 0");
}

void test_find_by_name() {
  const std::vector<display_layout_entry> list = two_monitors();
  expect(find_display_by_name(list, "\\\\.\\DISPLAY2") == 1,
         "a still-present panel keeps its new index");
  expect(find_display_by_name(list, "\\\\.\\DISPLAY9") == -1,
         "an unplugged panel is not found");
  expect(find_display_by_name(list, "") == -1,
         "an empty name never matches");
}

void test_reason_names() {
  expect(std::string(display_change_reason_name(
             display_change_reason::hotplug)) == "hotplug",
         "hotplug is named for the log");
  expect(std::string(display_change_reason_name(
             display_change_reason::resize)) == "resize",
         "resize is named for the log");
  expect(std::string(display_change_reason_name(
             display_change_reason::rotate)) == "rotate",
         "rotate is named for the log");
  expect(std::string(display_change_reason_name(
             display_change_reason::none)) == "none",
         "none is named for the log");
}

}  // namespace

int main() {
  test_identical_layout_does_not_trigger();
  test_count_change_is_hotplug();
  test_name_change_is_hotplug();
  test_size_change_is_resize();
  test_rotation_change_is_rotate();
  test_position_and_primary_are_hotplug();
  test_primary_selection();
  test_find_by_name();
  test_reason_names();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
