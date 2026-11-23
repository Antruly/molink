// Monitor list from `welcome.displays`, without Win32.
//
// The host serializes one item per capture display:
//   {"index":0,"name":"\\\\.\\DISPLAY1","x":0,"y":0,"width":1920,
//    "height":1080,"primary":true}
// and names the active one in `display_index`. The native shell needs this
// list for the toolbar selector; the rules that decide when a switch really
// resets the picture are pure and tested on Linux.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace molink::display {

struct info {
  int index = 0;
  /// UTF-8, exactly as the host named the monitor (may be empty).
  std::string name;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool primary = false;
};

struct welcome {
  std::vector<info> list;
  /// `welcome.display_index` when the host sent it, otherwise the first
  /// display (or 0).
  int current = 0;
  bool has_current = false;
};

/// Parses the `displays` array (and `display_index`) out of a welcome.
welcome parse_welcome(std::string_view json);

const info* find(const welcome& state, int index);

/// What a requested switch means for the local decoder. A request for the
/// display already active (or an index the host does not advertise) changes
/// nothing; a real switch discards the decoder state and asks for a fresh
/// keyframe.
struct switch_plan {
  bool changed = false;
  bool reset_picture = false;
  int index = 0;
};

switch_plan plan_switch(const welcome& state, int current, int next);

}  // namespace molink::display
