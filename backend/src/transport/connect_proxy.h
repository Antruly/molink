#pragma once

#include <functional>
#include <memory>

#include <webapp/uvcpp_web_ws.h>

#include "p2p/p2p_manager.h"
#include "port_forward.h"

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
/// `post` queues a callback on the core's event loop. It is required for AV1:
/// decoding runs on a worker thread and the rewritten packet must be delivered
/// from the loop, not from that thread.
/// `host_only` is read per connection so a settings change applies to the
/// next attempt. When it returns true the handler refuses the outbound
/// session with the `host_only_mode` close reason.
/// `port_forward` is the controller-side manager. Every /ws/connect session is
/// outbound (this core is the viewer), so it is attached here; a null manager
/// keeps port forwarding off (tests and reduced builds).
uvcpp::uvcpp_web_ws_handler make_connect_handler(
    std::shared_ptr<p2p_manager> manager,
    std::function<void(std::function<void()>)> post = {},
    std::function<bool()> host_only = {},
    std::shared_ptr<port_forward_manager> port_forward = {});

}  // namespace molink
