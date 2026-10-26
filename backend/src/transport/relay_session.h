#pragma once

#include <functional>
#include <string>

#include "transport/session_transport.h"

namespace molink {

/// The common surface of a relay-backed session transport, whatever protocol
/// carries it (WebSocket today, TCP/QUIC as they land).
///
/// The relay pairing is asynchronous: `start()` opens the connection and the
/// ready handler fires once the directory (or its relay endpoint) has paired
/// both devices. `is_open()` stays false until then, so callers can buffer.
class relay_session : public session_transport {
 public:
  ~relay_session() override = default;

  /// Called on the app loop when the peer is paired and data may flow.
  virtual void set_ready_handler(std::function<void()> ready) = 0;
  /// Begin connecting. Idempotent.
  virtual void start() = 0;
};

}  // namespace molink
