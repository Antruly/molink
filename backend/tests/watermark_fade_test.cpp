// Watermark proximity geometry and timing, without Win32. The shell feeds
// pointer moves and its 16 ms timer into the same functions.
#include <cmath>
#include <cstdio>

#include "watermark_fade.h"

namespace {

using molink::ui::advance_fade;
using molink::ui::fade_rect;
using molink::ui::fade_target;
using molink::ui::is_near_rect;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_close(float actual, float expected, const char* description) {
  ++checks;
  if (std::fabs(actual - expected) <= 1e-4f) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s (expected %.4f, got %.4f)\n", description,
               expected, actual);
}

void test_hit_rect_expands_by_28() {
  const fade_rect box{100, 100, 200, 140};
  // Inside, on the edge, and at the inclusive 28 px boundary.
  expect(is_near_rect(150, 120, box), "pointer inside the watermark rect");
  expect(is_near_rect(72, 120, box), "left edge minus exactly 28 matches");
  expect(is_near_rect(228, 120, box), "right edge plus exactly 28 matches");
  expect(is_near_rect(150, 72, box), "top edge minus exactly 28 matches");
  expect(is_near_rect(150, 168, box), "bottom edge plus exactly 28 matches");
  expect(is_near_rect(72, 72, box), "the expanded corner matches");
  expect(!is_near_rect(71.9f, 120, box), "past the left reach does not match");
  expect(!is_near_rect(150, 168.1f, box), "past the bottom reach does not match");
  expect(!is_near_rect(0, 0, box), "far away does not match");
}

void test_hit_rect_rejects_hidden_and_non_finite() {
  expect(!is_near_rect(0, 0, fade_rect{0, 0, 0, 0}),
         "a 0x0 hidden overlay never matches");
  expect(!is_near_rect(0, 0, fade_rect{10, 10, 5, 20}),
         "an inverted rectangle never matches");
  expect(!is_near_rect(std::nanf(""), 0, fade_rect{0, 0, 10, 10}),
         "a non-finite pointer never matches");
  expect(!is_near_rect(0, 0, fade_rect{0, std::nanf(""), 10, 10}),
         "a non-finite rectangle never matches");
  // A bad radius is clamped to zero rather than producing a huge hit area.
  expect(!is_near_rect(11, 5, fade_rect{0, 0, 10, 10}, std::nanf("")),
         "a non-finite radius reaches nowhere");
}

void test_fade_and_restore_timing() {
  fade_target badges;
  fade_target stats;

  // Enter the badges area and run the 16 ms timer for 80 ms.
  uint64_t now = 1000;
  bool changed = advance_fade(&badges, true, now, 16);
  expect(changed, "the first near frame changes the opacity");
  for (int i = 0; i < 4; ++i) {
    now += 16;
    advance_fade(&badges, true, now, 16);
  }
  expect_close(badges.alpha, molink::ui::kWatermarkFadeOpacity,
               "80 ms of proximity fades the badges to 0.08");
  expect_close(stats.alpha, 1.0f,
               "the stats bar is unaffected while the pointer is at badges");

  // Leave: the overlay holds the faded value for the debounce window.
  changed = advance_fade(&badges, false, now, 16);
  expect(!changed, "leaving does not restore immediately");
  expect(badges.pending_restore, "leaving schedules a restore");
  expect_close(badges.alpha, molink::ui::kWatermarkFadeOpacity,
               "the faded value is held during the debounce");
  now = badges.restore_at - 1;
  advance_fade(&badges, false, now, 16);
  expect_close(badges.alpha, molink::ui::kWatermarkFadeOpacity,
               "still faded just before the debounce ends");

  // At the deadline the 80 ms transition back starts.
  now = badges.restore_at;
  changed = advance_fade(&badges, false, now, 16);
  expect(changed, "restoring changes the opacity");
  for (int i = 0; i < 4; ++i) {
    now += 16;
    advance_fade(&badges, false, now, 16);
  }
  expect_close(badges.alpha, 1.0f, "the overlay is fully restored after 80 ms");
  expect(!badges.pending_restore, "the restore debounce is consumed");
}

void test_targets_are_independent() {
  fade_target badges;
  fade_target stats;
  uint64_t now = 10;

  advance_fade(&badges, true, now, 16);
  advance_fade(&stats, false, now, 16);
  for (int i = 0; i < 5; ++i) {
    now += 16;
    advance_fade(&badges, true, now, 16);
    advance_fade(&stats, false, now, 16);
  }
  expect(badges.alpha < 0.1f, "the hovered target fades");
  expect_close(stats.alpha, 1.0f, "the other target stays fully opaque");

  // Moving straight from one overlay to the other clears the pending
  // restore of the first and starts fading the second.
  advance_fade(&badges, false, now, 16);
  expect(badges.pending_restore, "the first target is waiting to restore");
  advance_fade(&stats, true, now, 16);
  expect(stats.alpha < 1.0f &&
             stats.alpha > molink::ui::kWatermarkFadeOpacity,
         "entering the second target fades from its current opacity");
  for (int i = 0; i < 5; ++i) {
    now += 16;
    advance_fade(&badges, true, now, 16);
    advance_fade(&stats, true, now, 16);
  }
  expect(!badges.pending_restore,
         "re-entering the first target cancels its restore");
  expect_close(badges.alpha, molink::ui::kWatermarkFadeOpacity,
               "the first target is faded again");
  expect_close(stats.alpha, molink::ui::kWatermarkFadeOpacity,
               "the second target fades independently");
}

void test_zero_elapsed_does_not_jump() {
  fade_target target;
  advance_fade(&target, true, 0, 0);
  expect_close(target.alpha, 1.0f,
               "a zero-length frame does not change the opacity");
  advance_fade(&target, true, 100, 1000);
  expect_close(target.alpha, molink::ui::kWatermarkFadeOpacity,
               "a long frame clamps to the full transition");
}

}  // namespace

int main() {
  test_hit_rect_expands_by_28();
  test_hit_rect_rejects_hidden_and_non_finite();
  test_fade_and_restore_timing();
  test_targets_are_independent();
  test_zero_elapsed_does_not_jump();
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("watermark_fade: %d checks passed\n", checks);
  return 0;
}
