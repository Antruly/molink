#include "input/input_event.h"

#include <string>
#include <utility>

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

}  // namespace

input_injector::status inject_input_event(input_injector& input,
                                          const input_event& event,
                                          std::string& err,
                                          secure_attention_fn sas) {
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
    case input_event::kind::wheel:
      return finish(input.wheel(static_cast<int>(event.dx),
                                static_cast<int>(event.dy)),
                    err, "wheel delta rejected");
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
