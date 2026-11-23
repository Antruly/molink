// Relative mouse accumulation and sensitivity conversion, without Win32.
#include <cmath>
#include <cstdio>

#include "trackpad_mouse.h"

namespace {

using namespace molink::mouse;

int checks = 0;
int failures = 0;

void expect(bool value, const char* description) {
  ++checks;
  if (value) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", description);
}

void expect_close(double actual, double expected, const char* description) {
  ++checks;
  if (std::fabs(actual - expected) <= 1e-9) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s (expected %.6f, got %.6f)\n", description,
               expected, actual);
}

void test_web_sensitivity_moves_a_fraction_of_the_picture() {
  cursor position;
  const move_result moved =
      advance(&position, 100.0, 0.0, 1000.0, 500.0, kDefaultSensitivity);
  expect(moved.changed, "a nonzero delta moves the virtual cursor");
  expect_close(moved.x, 0.5 + 0.17, "100 px of 1000 at 1.7x");
  expect_close(moved.y, 0.5, "a horizontal delta leaves y alone");
  const move_result vertical =
      advance(&position, 0.0, 50.0, 1000.0, 500.0, kDefaultSensitivity);
  expect_close(vertical.y, 0.5 + 0.17, "y uses the picture height");
}

void test_sensitivity_choices() {
  expect_close(sensitivity_at(0), 0.8, "choice 0");
  expect_close(sensitivity_at(1), 1.2, "choice 1");
  expect_close(sensitivity_at(2), kDefaultSensitivity, "choice 2 is the web");
  expect_close(sensitivity_at(3), 2.5, "choice 3");
  expect_close(sensitivity_at(4), 3.5, "choice 4");
  expect(sensitivity_index(1.7) == 2, "1.7 maps back to the default row");
  expect(sensitivity_index(0.01) == 0, "values below the table clamp low");
  expect(sensitivity_index(99.0) == 4, "values above the table clamp high");
  expect_close(clamp_sensitivity(-1.0), 0.2, "sensibility clamps at 0.2");
  expect_close(clamp_sensitivity(100.0), 5.0, "sensibility clamps at 5");
}

void test_clamps_to_the_picture_edges() {
  cursor position;
  advance(&position, 10000.0, 10000.0, 1000.0, 1000.0, 1.7);
  expect_close(position.x, 1.0, "x clamps at the right edge");
  expect_close(position.y, 1.0, "y clamps at the bottom edge");
  advance(&position, -10000.0, -10000.0, 1000.0, 1000.0, 1.7);
  expect_close(position.x, 0.0, "x clamps at the left edge");
  expect_close(position.y, 0.0, "y clamps at the top edge");
}

void test_zero_and_non_finite_deltas_are_ignored() {
  cursor position;
  position.x = 0.25;
  position.y = 0.75;
  const move_result still = advance(&position, 0.0, 0.0, 800.0, 600.0, 1.7);
  expect(!still.changed, "a zero delta reports no movement");
  expect_close(position.x, 0.25, "a zero delta keeps x");
  expect_close(position.y, 0.75, "a zero delta keeps y");
  advance(&position, std::nan(""), std::nan(""), 800.0, 600.0, 1.7);
  expect_close(position.x, 0.25, "NaN keeps x");
  expect_close(position.y, 0.75, "NaN keeps y");
}

void test_higher_sensitivity_moves_further() {
  cursor slow;
  cursor fast;
  advance(&slow, 100.0, 0.0, 1000.0, 1000.0, 0.8);
  advance(&fast, 100.0, 0.0, 1000.0, 1000.0, 3.5);
  expect(fast.x > slow.x, "a higher sensitivity moves the cursor further");
}

}  // namespace

int main() {
  test_web_sensitivity_moves_a_fraction_of_the_picture();
  test_sensitivity_choices();
  test_clamps_to_the_picture_edges();
  test_zero_and_non_finite_deltas_are_ignored();
  test_higher_sensitivity_moves_further();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
