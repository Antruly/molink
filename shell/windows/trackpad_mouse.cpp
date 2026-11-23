#include "trackpad_mouse.h"

#include <algorithm>
#include <cmath>

namespace molink::mouse {
namespace {

constexpr double kChoices[sensitivity_choice_count] = {
    0.8, 1.2, 1.7, 2.5, 3.5};

double clamp_unit(double value) {
  if (!(value > 0.0)) return 0.0;  // also catches NaN
  if (value > 1.0) return 1.0;
  return value;
}

}  // namespace

double clamp_sensitivity(double value) {
  if (!std::isfinite(value)) return kDefaultSensitivity;
  return std::max(0.2, std::min(5.0, value));
}

double sensitivity_at(int index) {
  if (index < 0) index = 0;
  if (index >= sensitivity_choice_count) index = sensitivity_choice_count - 1;
  return kChoices[index];
}

int sensitivity_index(double value) {
  const double clamped = clamp_sensitivity(value);
  int best = 0;
  double distance = std::fabs(clamped - kChoices[0]);
  for (int i = 1; i < sensitivity_choice_count; ++i) {
    const double next = std::fabs(clamped - kChoices[i]);
    if (next < distance) {
      distance = next;
      best = i;
    }
  }
  return best;
}

move_result advance(cursor* position, double dx, double dy, double width,
                    double height, double sensitivity) {
  move_result result;
  if (position == nullptr) return result;
  const double sx = clamp_sensitivity(sensitivity);
  const double span_x = width > 1.0 ? width : 1.0;
  const double span_y = height > 1.0 ? height : 1.0;
  double x = position->x;
  double y = position->y;
  if (std::isfinite(dx)) x += (dx / span_x) * sx;
  if (std::isfinite(dy)) y += (dy / span_y) * sx;
  x = clamp_unit(x);
  y = clamp_unit(y);
  result.x = x;
  result.y = y;
  result.changed = x != position->x || y != position->y;
  position->x = x;
  position->y = y;
  return result;
}

}  // namespace molink::mouse
