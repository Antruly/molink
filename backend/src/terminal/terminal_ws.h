#pragma once

#include <memory>

#include <webapp/uvcpp_web_ws.h>

#include "terminal/terminal_service.h"

namespace molink {

class auth_state;

/// WS /ws/terminal?token=<session-token>
///
/// Control messages are JSON text; terminal bytes are binary frames whose
/// first byte is the frame kind (1 = output, 2 = input). Output frames carry
/// the terminal id and the sequence number of their first byte, so a viewer
/// can re-attach with `term-attach` and replay from where it left off. Nothing
/// is base64-encoded.
///
/// The host-global terminal_service keeps sessions alive across viewer
/// disconnects; this handler only owns the connection-scoped viewer identity,
/// its listener subscription and the set of ids it is currently attached to.
uvcpp::uvcpp_web_ws_handler make_terminal_handler(
    std::shared_ptr<terminal_service> service,
    std::shared_ptr<auth_state> auth);

}  // namespace molink
