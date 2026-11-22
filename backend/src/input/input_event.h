#pragma once

#include <string>

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

/// Executes one event.
///
/// On `rejected` the error is always filled in, including the key code the
/// host did not understand. Callers can put it straight into the `bad_input`
/// error frame instead of sending an empty message the UI cannot explain.
input_injector::status inject_input_event(input_injector& input,
                                          const input_event& event,
                                          std::string& err,
                                          secure_attention_fn sas = nullptr);

}  // namespace molink
