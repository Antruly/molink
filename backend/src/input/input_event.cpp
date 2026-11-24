#include "input/input_event.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace molink {
namespace {

input_injector::status reject(std::string& err, std::string message) {
  err = std::move(message);
  return input_injector::status::rejected;
}

/// A platform injector may reject a value without filling the caller's error
/// (the rejected status is enough for its own callers). Never let that reach
/// the wire as an empty `bad_input`.
input_injector::status finish(input_injector::status status, std::string& err,
                              const std::string& fallback) {
  if (status == input_injector::status::rejected && err.empty()) {
    err = fallback;
  }
  return status;
}

input_event key_up(std::string code) {
  input_event event;
  event.type = input_event::kind::key;
  event.code = std::move(code);
  event.down = false;
  return event;
}

}  // namespace

wheel_accumulator::notches wheel_accumulator::add(double dx, double dy) {
  // Clamp the accumulator, not just the output: a hostile or broken client
  // cannot build up minutes of scrolling that would keep firing after it
  // stopped. The clamp also matches the injector's per-call limit.
  constexpr double kLimit = 10.0;
  x_ = std::clamp(x_ + dx, -kLimit, kLimit);
  y_ = std::clamp(y_ + dy, -kLimit, kLimit);
  notches out;
  out.dx = static_cast<int>(std::lround(x_));
  out.dy = static_cast<int>(std::lround(y_));
  x_ -= out.dx;
  y_ -= out.dy;
  return out;
}

void wheel_accumulator::reset() {
  x_ = 0.0;
  y_ = 0.0;
}

bool held_key_state::is_modifier(std::string_view code) {
  // Aliases accepted from older browsers (OSLeft/OSRight, Win, Super).
  static constexpr std::string_view kModifiers[] = {
      "ShiftLeft",  "ShiftRight", "ControlLeft", "ControlRight", "AltLeft",
      "AltRight",   "MetaLeft",   "MetaRight",   "AltGraph",     "OSLeft",
      "OSRight",    "Win",        "Super",
  };
  for (const std::string_view modifier : kModifiers) {
    if (code == modifier) return true;
  }
  return false;
}

void held_key_state::note(const std::string& code, bool down,
                          std::uint64_t now_ms) {
  if (code.empty()) return;
  if (!down) {
    for (auto it = held_.begin(); it != held_.end(); ++it) {
      if (it->code == code) {
        held_.erase(it);
        return;
      }
    }
    return;
  }
  for (entry& held : held_) {
    if (held.code == code) {
      // Auto-repeat proves the key is still down; extend its lease.
      held.down_ms = now_ms;
      return;
    }
  }
  held_.push_back({code, now_ms});
}

std::vector<input_event> held_key_state::release_all() {
  std::vector<input_event> events;
  events.reserve(held_.size());
  for (auto it = held_.rbegin(); it != held_.rend(); ++it) {
    events.push_back(key_up(it->code));
  }
  held_.clear();
  return events;
}

std::vector<input_event> held_key_state::expire(std::uint64_t now_ms,
                                                std::uint64_t timeout_ms) {
  std::vector<input_event> events;
  for (auto it = held_.begin(); it != held_.end();) {
    if (is_modifier(it->code) && now_ms > it->down_ms &&
        now_ms - it->down_ms >= timeout_ms) {
      events.push_back(key_up(it->code));
      it = held_.erase(it);
    } else {
      ++it;
    }
  }
  return events;
}

bool held_key_state::held(std::string_view code) const {
  for (const entry& held : held_) {
    if (held.code == code) return true;
  }
  return false;
}

input_injector::status inject_input_event(input_injector& input,
                                          const input_event& event,
                                          std::string& err,
                                          secure_attention_fn sas,
                                          wheel_accumulator* wheel) {
  err.clear();
  switch (event.type) {
    case input_event::kind::sas: {
      if (sas == nullptr) return reject(err, "secure attention is unavailable");
      std::string sas_error;
      if (!sas(sas_error)) {
        return reject(err, sas_error.empty() ? "secure attention failed"
                                             : sas_error);
      }
      return input_injector::status::ok;
    }
    case input_event::kind::move:
      if (!event.has_xy) return reject(err, "move needs normalized x and y");
      return finish(input.move(event.x, event.y), err,
                    "move coordinates are outside 0..1");
    case input_event::kind::button:
      if (!event.has_button) return reject(err, "button needs a button number");
      return finish(
          input.button(event.button, event.down), err,
          "unknown mouse button: " + std::to_string(event.button));
    case input_event::kind::wheel: {
      wheel_accumulator::notches notches;
      if (wheel != nullptr) {
        notches = wheel->add(event.dx, event.dy);
      } else {
        notches.dx = static_cast<int>(std::lround(event.dx));
        notches.dy = static_cast<int>(std::lround(event.dy));
      }
      return finish(input.wheel(notches.dx, notches.dy), err,
                    "wheel delta rejected");
    }
    case input_event::kind::key:
      if (event.code.empty()) return reject(err, "key needs a code");
      return finish(input.key(event.code, event.down), err,
                    "unknown key code: " + event.code);
    case input_event::kind::text:
      if (event.text.empty() || event.text.size() > 4096) {
        return reject(err, "text must be 1..4096 bytes");
      }
      return finish(input.text(event.text), err,
                    "text could not be injected");
    case input_event::kind::unknown:
      return reject(err,
                    "unknown input event kind: " + event.kind_name);
  }
  return reject(err, "unknown input event kind");
}

}  // namespace molink
