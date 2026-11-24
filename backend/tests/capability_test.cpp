// Capability presets and the wire names. Pure data, no transport.
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include "capabilities.h"

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

const capability kAll[] = {
    capability::screen,          capability::input,
    capability::clipboard_read,  capability::clipboard_write,
    capability::files_read,      capability::files_write,
    capability::audio,           capability::chat,
    capability::logs,            capability::restart,
};

void test_full_policy() {
  const uint32_t mask = capabilities_for_policy("full");
  expect(mask == kCapabilityAll, "full grants every bit");
  for (capability value : kAll) {
    expect(has_capability(mask, value), "full has every capability");
  }
  // An unknown value is treated as the backward-compatible default.
  expect(capabilities_for_policy("garbage") == kCapabilityAll,
         "unknown policy falls back to full");
  expect(capabilities_for_policy("") == kCapabilityAll,
         "empty policy falls back to full");
}

void test_view_only_policy() {
  const uint32_t mask = capabilities_for_policy("view_only");
  expect(has_capability(mask, capability::screen), "view_only sees the screen");
  expect(has_capability(mask, capability::audio), "view_only may hear audio");
  const capability denied[] = {
      capability::input,         capability::clipboard_read,
      capability::clipboard_write, capability::files_read,
      capability::files_write,   capability::chat,
      capability::logs,          capability::restart,
  };
  for (capability value : denied) {
    expect(!has_capability(mask, value),
           "view_only denies everything except screen and audio");
  }
  expect(mask == (capability_bit(capability::screen) |
                  capability_bit(capability::audio)),
         "view_only mask is exactly screen|audio");
}

void test_assist_policy() {
  const uint32_t mask = capabilities_for_policy("assist");
  expect(has_capability(mask, capability::screen), "assist sees the screen");
  expect(has_capability(mask, capability::input), "assist may use input");
  const capability denied[] = {
      capability::clipboard_read, capability::clipboard_write,
      capability::files_read,     capability::files_write,
      capability::audio,          capability::chat,
      capability::logs,           capability::restart,
  };
  for (capability value : denied) {
    expect(!has_capability(mask, value),
           "assist denies clipboard, files, audio, chat and logs");
  }
  expect(mask == (capability_bit(capability::screen) |
                  capability_bit(capability::input)),
         "assist mask is exactly screen|input");
}

void test_names_and_validation() {
  std::set<std::string> names;
  for (capability value : kAll) {
    const std::string name = capability_name(value);
    expect(!name.empty() && name != "unknown", "capability has a name");
    expect(names.insert(name).second, "capability names are unique");
    expect(capability_bit(value) != 0, "capability bit is non-zero");
  }
  // Every bit is distinct.
  uint32_t combined = 0;
  for (capability value : kAll) {
    expect((combined & capability_bit(value)) == 0,
           "capability bits do not overlap");
    combined |= capability_bit(value);
  }
  expect(combined == kCapabilityAll, "the ten bits fill the mask");
  expect(valid_access_policy("full"), "full is a valid policy");
  expect(valid_access_policy("view_only"), "view_only is a valid policy");
  expect(valid_access_policy("assist"), "assist is a valid policy");
  expect(!valid_access_policy("friendly"), "unknown policy is invalid");
  expect(!valid_access_policy(""), "empty policy name is invalid");
}

}  // namespace

int main() {
  test_full_policy();
  test_view_only_policy();
  test_assist_policy();
  test_names_and_validation();
  if (failures == 0) {
    std::printf("capabilities: %d checks passed\n", checks);
    return 0;
  }
  std::fprintf(stderr, "capabilities: %d/%d checks failed\n", failures,
               checks);
  return 1;
}
