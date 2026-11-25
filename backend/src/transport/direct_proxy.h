#pragma once

#include <functional>
#include <memory>

#include <webapp/uvcpp_web_ws.h>

#include "port_forward.h"

namespace uvcpp {
class uvcpp_loop;
}

namespace molink {

/// WS /ws/direct?host=<ip>&port=<port>
///
/// Bridges the browser to a host session with the same AV1 decoding as the
/// device-ID proxy. Typed-IP sessions use this so the client core stays in the
/// path and can turn AV1 patches into the BGRA tiles the page paints.
///
/// The upstream leg asks the host's `/api/transports` for an available QUIC
/// data channel and uses it when present; otherwise it falls back to the plain
/// `/ws/session` WebSocket this proxy has always used. `loop` owns the QUIC
/// client's UDP socket, so it must be the app loop. Local-only: the page is
/// served by this core.
/// `host_only` is read per connection. In host_only mode only a loopback
/// target is accepted; anything else is closed with `host_only_mode`.
/// `port_forward` is attached to the upstream session for the lifetime of the
/// proxy, like /ws/connect: both proxies exist only when this core is the
/// viewer.
uvcpp::uvcpp_web_ws_handler make_direct_handler(
    std::function<void(std::function<void()>)> post, uvcpp::uvcpp_loop* loop,
    std::function<bool()> host_only = {},
    std::shared_ptr<port_forward_manager> port_forward = {});

}  // namespace molink
