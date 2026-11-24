#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "input/input.h"

namespace molink {

/// One decoded input event, independent of the JSON library the transport
/// uses. Keeping the validation and the injected errors here makes the
/// "rejected must carry a message" rule testable without a live host.
struct input_event {
  enum class kind { sas, move, button, wheel, key, text, unknown };

  kind type = kind::unknown;
  /// The raw `k` value; only used to describe an unknown kind.
  std::string kind_name;
  std::string code;
  std::string text;
  double x = 0.0;
  double y = 0.0;
  double dx = 0.0;
  double dy = 0.0;
  int button = 1;
  bool down = true;
  bool has_xy = false;
  bool has_button = false;
};

/// Ctrl+Alt+Del cannot be synthesized with SendInput; the platform layer that
/// can do it supplies this hook. Returning false fills @p err.
using secure_attention_fn = bool (*)(std::string& err);

/// A native injector takes whole wheel notches. Browsers and the native shell
/// report fractions of one (a 40 px trackpad step is 1/3 of a 120 px notch),
/// so the remainder is carried to the next event instead of being truncated
/// away.
class wheel_accumulator {
 public:
  struct notches {
    int dx = 0;
    int dy = 0;
  };

  /// Adds one event's fractional notches and returns the whole notches now
  /// due. The fractional remainder stays in the accumulator.
  notches add(double dx, double dy);
  void reset();

 private:
  double x_ = 0.0;
  double y_ = 0.0;
};

/// One key a session is holding on the host. The state is per session so a
/// disconnect releases only the keys that session pressed, not a modifier a
/// second viewer is still holding.
class held_key_state {
 public:
  /// True for Shift/Control/Alt/Meta/AltGraph, the keys a lost key-up leaves
  /// visibly stuck. Auto-repeat only re-sends a key-down; it never produces
  /// the missing key-up.
  static bool is_modifier(std::string_view code);

  /// Records a key-down the injector accepted, or removes a released code.
  /// A duplicate key-down (auto-repeat) refreshes the timestamp.
  void note(const std::string& code, bool down, std::uint64_t now_ms);

  /// Key-up events for everything this session holds, newest first.
  std::vector<input_event> release_all();

  /// Key-up events for modifiers held longer than @p timeout_ms. Used as the
  /// half-open-connection fallback: the transport may never report a close.
  std::vector<input_event> expire(std::uint64_t now_ms,
                                  std::uint64_t timeout_ms);

  bool held(std::string_view code) const;
  std::size_t size() const { return held_.size(); }
  void clear() { held_.clear(); }

 private:
  struct entry {
    std::string code;
    std::uint64_t down_ms = 0;
  };
  std::vector<entry> held_;
};

/// Executes one event.
///
/// On `rejected` the error is always filled in, including the key code the
/// host did not understand. Callers can put it straight into the `bad_input`
/// error frame instead of sending an empty message the UI cannot explain.
///
/// @p wheel carries the fractional wheel remainder between events; nullptr
/// rounds each event on its own.
input_injector::status inject_input_event(input_injector& input,
                                          const input_event& event,
                                          std::string& err,
                                          secure_attention_fn sas = nullptr,
                                          wheel_accumulator* wheel = nullptr);

}  // namespace molink
