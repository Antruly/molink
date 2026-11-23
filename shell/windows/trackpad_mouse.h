// Relative (trackpad / virtual mouse) input, without Win32.
//
// The host protocol has no relative pointer event: the Web's virtual mouse
// accumulates pointer deltas into its own cursor and sends the ordinary
// normalized `move` (see `onTouchpadMove` in SessionView.tsx). The native
// shell does exactly the same, so the wire stays byte-compatible and an
// unmodified host needs no new input field.
#pragma once

namespace molink::mouse {

/// The Web's sensitivity for the touch/virtual mouse. The shell lets the user
/// change it; 1.7 stays the default so a fresh install feels like the page.
inline constexpr double kDefaultSensitivity = 1.7;

struct cursor {
  double x = 0.5;
  double y = 0.5;
};

struct move_result {
  double x = 0.5;
  double y = 0.5;
  bool changed = false;
};

double clamp_sensitivity(double value);

/// Sensitivity choices shown in the settings panel.
inline constexpr int sensitivity_choice_count = 5;

double sensitivity_at(int index);
int sensitivity_index(double value);

/// Adds a pointer delta to the virtual cursor. `width` and `height` are the
/// picture's size in the current input units (DIPs or pixels); a delta of the
/// full width at sensitivity 1 moves the cursor by exactly one screen.
/// Coordinates clamp to 0..1, and a zero delta leaves `changed` false.
move_result advance(cursor* position, double dx, double dy, double width,
                    double height, double sensitivity);

}  // namespace molink::mouse
