#pragma once

#include <functional>
#include <memory>

#include <webapp/uvcpp_web_ws.h>

namespace molink {

/// WS /ws/direct?host=<ip>&port=<port>
///
/// Bridges the browser to a host's `/ws/session` over a plain WebSocket, with
/// the same AV1 decoding as the device-ID proxy. Typed-IP sessions use this so
/// the client core stays in the path and can turn AV1 patches into the BGRA
/// tiles the page paints. Local-only: the page is served by this core.
uvcpp::uvcpp_web_ws_handler make_direct_handler(
    std::function<void(std::function<void()>)> post);

}  // namespace molink
