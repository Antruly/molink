#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "port_forward.h"
#include "transport/session_transport.h"

namespace molink {

/// Controller-side glue between one outbound proxy session and the local
/// `port_forward_manager`.
///
/// The `/ws/connect` and `/ws/direct` proxies are only created when this core
/// opens a session *as the viewer*: that is the existing role signal, so no
/// extra "am I the controller" flag is stored. The bridge keeps the port
/// forwarding control messages and FORWARD_DATA packets off the page's
/// WebSocket while feeding them to the manager, and it sends the manager's
/// bytes back over the same session channel.
class port_forward_bridge {
 public:
  using post_fn = std::function<void(std::function<void()>)>;

  port_forward_bridge(std::shared_ptr<port_forward_manager> manager,
                      std::shared_ptr<session_transport> transport,
                      post_fn post = {});
  ~port_forward_bridge();

  port_forward_bridge(const port_forward_bridge&) = delete;
  port_forward_bridge& operator=(const port_forward_bridge&) = delete;

  /// Installs the link on the manager. Idempotent; safe to call once the
  /// transport is ready. Closing the session must call detach().
  void attach();
  /// Removes this session's link and closes its live streams. Listeners stay
  /// bound, so the next local connection works again after a reconnect.
  void detach();

  /// True when @p text is a forward control message (and was consumed).
  bool handle_text(const std::string& text);
  /// True when @p data is a FORWARD_DATA packet (and was consumed).
  bool handle_binary(const uint8_t* data, std::size_t len);

 private:
  struct link;

  std::shared_ptr<port_forward_manager> manager_;
  std::shared_ptr<session_transport> transport_;
  post_fn post_;
  std::shared_ptr<link> link_;
  std::atomic<bool> attached_{false};
};

}  // namespace molink
