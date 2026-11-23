#include "watermark_fade.h"

#include <algorithm>
#include <cmath>

namespace molink::ui {

bool is_near_rect(float x, float y, const fade_rect& rect, float radius) {
  if (!std::isfinite(x) || !std::isfinite(y)) return false;
  if (!std::isfinite(rect.left) || !std::isfinite(rect.top) ||
      !std::isfinite(rect.right) || !std::isfinite(rect.bottom)) {
    return false;
  }
  // A hidden element reports 0x0 at (0, 0); never fade something invisible.
  if (!(rect.right > rect.left) || !(rect.bottom > rect.top)) return false;
  const float reach = std::isfinite(radius) ? (std::max)(0.0f, radius) : 0.0f;
  return x >= rect.left - reach && x <= rect.right + reach &&
         y >= rect.top - reach && y <= rect.bottom + reach;
}

bool advance_fade(fade_target* target, bool near_pointer, uint64_t now_ms,
                  uint64_t elapsed_ms) {
  if (target == nullptr) return false;
  const float before = target->alpha;

  if (near_pointer) {
    target->is_near = true;
    target->pending_restore = false;
    target->restore_at = 0;
  } else if (target->is_near) {
    target->is_near = false;
    target->pending_restore = true;
    target->restore_at =
        now_ms + static_cast<uint64_t>(kWatermarkRestoreDelayMs);
  }

  float goal = 1.0f;
  if (target->is_near) {
    goal = kWatermarkFadeOpacity;
  } else if (target->pending_restore) {
    if (now_ms >= target->restore_at) {
      target->pending_restore = false;
      goal = 1.0f;
    } else {
      // Hold the faded value during the debounce.
      goal = target->alpha;
    }
  }

  if (goal != target->alpha && elapsed_ms > 0) {
    const float fraction =
        elapsed_ms >= kWatermarkFadeDurationMs
            ? 1.0f
            : static_cast<float>(elapsed_ms) / kWatermarkFadeDurationMs;
    const float step = (1.0f - kWatermarkFadeOpacity) * fraction;
    if (target->alpha < goal) {
      target->alpha = (std::min)(goal, target->alpha + step);
    } else {
      target->alpha = (std::max)(goal, target->alpha - step);
    }
  }

  return std::fabs(target->alpha - before) > 1e-6f;
}

}  // namespace molink::ui
