#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace molink {

/// Why a display layout changed. The capture backends classify a change here
/// so the log and the wire message say what happened, not just "something
/// moved".
enum class display_change_reason : unsigned char {
  none,
  hotplug,  ///< a monitor was added/removed, or the layout was rearranged
  resize,   ///< the desktop mode of an existing monitor changed
  rotate,   ///< an existing monitor was rotated
};

inline const char* display_change_reason_name(display_change_reason reason) {
  switch (reason) {
    case display_change_reason::hotplug:
      return "hotplug";
    case display_change_reason::resize:
      return "resize";
    case display_change_reason::rotate:
      return "rotate";
    case display_change_reason::none:
      break;
  }
  return "none";
}

/// The fields that decide whether a hotplug/resize/rotate change happened.
/// Deliberately independent of the platform and of display_info so the
/// comparison is a pure function both backends can share and a unit test can
/// drive with fake lists.
struct display_layout_entry {
  /// Stable per-monitor identifier (DXGI device name on Windows, XRandR
  /// monitor name on X11).
  std::string name;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  /// DXGI_MODE_ROTATION value on Windows. X11 leaves this 0: a rotated
  /// monitor already reports its rotated width/height.
  int rotation = 0;
  bool primary = false;
};

/// Compares two positional monitor lists. Returning `none` means nothing a
/// viewer can observe has changed; every other value is a reason to rebuild
/// the capture and tell the clients.
inline display_change_reason compare_display_layouts(
    const std::vector<display_layout_entry>& before,
    const std::vector<display_layout_entry>& after) {
  if (before.size() != after.size()) return display_change_reason::hotplug;

  bool moved = false;
  bool resized = false;
  bool rotated = false;
  for (std::size_t i = 0; i < before.size(); ++i) {
    const display_layout_entry& a = before[i];
    const display_layout_entry& b = after[i];
    // A different monitor at the same position means one was unplugged and
    // another took its place (or the enumeration order changed).
    if (a.name != b.name) return display_change_reason::hotplug;
    if (a.rotation != b.rotation) rotated = true;
    if (a.width != b.width || a.height != b.height) resized = true;
    // A rearrangement or a primary switch moves every click; it is not a
    // resolution change, so report it as a hotplug.
    if (a.x != b.x || a.y != b.y || a.primary != b.primary) moved = true;
  }

  // Rotation can also change the reported width/height; report the more
  // specific cause first.
  if (rotated) return display_change_reason::rotate;
  if (resized) return display_change_reason::resize;
  if (moved) return display_change_reason::hotplug;
  return display_change_reason::none;
}

/// Index of the monitor the OS flags as primary. Windows decides this with
/// MONITORINFOF_PRIMARY, X11 with the XRandR primary flag. Falls back to 0 so
/// a caller that needs a valid selection still gets one; an empty list also
/// yields 0.
inline int primary_display_index(
    const std::vector<display_layout_entry>& displays) {
  for (std::size_t i = 0; i < displays.size(); ++i) {
    if (displays[i].primary) return static_cast<int>(i);
  }
  return 0;
}

/// Index of the monitor named @p name, or -1 when it is gone. Used to keep
/// capturing the same panel when the list is re-enumerated and indices shift.
inline int find_display_by_name(
    const std::vector<display_layout_entry>& displays,
    const std::string& name) {
  if (name.empty()) return -1;
  for (std::size_t i = 0; i < displays.size(); ++i) {
    if (displays[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace molink
