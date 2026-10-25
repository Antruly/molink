#pragma once

#include <memory>

#include <webapp/uvcpp_web_ws.h>

#include "p2p/p2p_manager.h"

namespace molink {

/// WS /ws/connect?target=<device-id>
///
/// The browser cannot open UDP sockets or run ICE, so it connects to the local
/// core instead. This handler bridges that WebSocket to an ICE-selected UDP
/// transport to the target core. The remote core attaches a normal
/// session_controller to the other end, so hello/welcome and every MOLK packet
/// keep working unchanged.
///
/// If ICE is unavailable the connection is closed; the browser falls back to a
/// direct WebSocket to the address it resolved through the directory.
uvcpp::uvcpp_web_ws_handler make_connect_handler(
    std::shared_ptr<p2p_manager> manager);

}  // namespace molink
