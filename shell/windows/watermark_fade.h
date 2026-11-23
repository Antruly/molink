// Proximity fade for the session watermark.
//
// The Web fades each overlay (corner badges, bottom stats bar, connecting
// hint) independently when the pointer is inside its rectangle expanded by
// 28 px, holds the faded state for a short debounce, then restores over an
// 80 ms transition. This header is intentionally free of Win32 so the
// geometry and the timing can be tested on Linux.
#pragma once

#include <cstdint>

namespace molink::ui {

/// A rectangle in the same coordinate space as the pointer (client DIPs).
struct fade_rect {
  float left = 0;
  float top = 0;
  float right = 0;
  float bottom = 0;
};

inline constexpr float kWatermarkFadeRadius = 28.0f;
inline constexpr float kWatermarkFadeOpacity = 0.08f;
inline constexpr float kWatermarkFadeDurationMs = 80.0f;
inline constexpr float kWatermarkRestoreDelayMs = 80.0f;

/// True when (x, y) is inside `rect` expanded by `radius` on every side.
/// Boundaries are inclusive. Degenerate rectangles (a hidden overlay reports
/// 0x0) never match, and non-finite input never matches.
bool is_near_rect(float x, float y, const fade_rect& rect,
                  float radius = kWatermarkFadeRadius);

/// One overlay's fade state. `alpha` is the opacity the painter multiplies
/// into the overlay's colours; `near` is the pointer verdict; the restore
/// fields hold the debounce before the overlay comes back.
struct fade_target {
  float alpha = 1.0f;
  bool is_near = false;
  bool pending_restore = false;
  uint64_t restore_at = 0;
};

/// Advances one target. Call from the UI timer (and from pointer moves) with
/// the elapsed milliseconds since the previous call. Returns true when the
/// opacity changed, so the caller can repaint. When the pointer stays near,
/// the alpha eases to `kWatermarkFadeOpacity` in `kWatermarkFadeDurationMs`;
/// after leaving, it stays faded for `kWatermarkRestoreDelayMs` and then eases
/// back to 1. Each target is independent.
bool advance_fade(fade_target* target, bool near_pointer, uint64_t now_ms,
                  uint64_t elapsed_ms);

}  // namespace molink::ui
