#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "transport/session_transport.h"

namespace molink {

class session_controller;

struct session_info {
  uint32_t id = 0;
  std::string public_id;      ///< e.g. "A3F-9K2-X7Q", the molink://session id
  std::string peer_ip;
  unsigned int peer_port = 0;
  bool authenticated = false;
  /// The client advertised AV1 support in hello (the connect proxy injects it
  /// when its core was built with dav1d). AV1 patches are only produced when
  /// every authenticated session supports them.
  bool av1 = false;
  /// The client advertised `seq_clock` in hello: it understands the per-cell
  /// applied-sequence map and can name the cells it missed.
  bool seq_clock = false;
  /// Last time (monotonic ms) this session received a sequence map, so a
  /// polling client is not flooded while the screen is static.
  uint64_t last_seq_ms = 0;
  /// What the viewer last reported about its own pipeline (see the "viewer"
  /// control message). Freshness is tracked so a silent viewer stops counting.
  double viewer_paint_fps = 0;
  int viewer_dropped = 0;
  double viewer_rx_rate = 0;
  uint64_t viewer_stats_ms = 0;
  /// Identity the client reported in hello; lets a shared chat label who sent
  /// each message when several viewers are connected.
  std::string client_name;
  std::string client_id;
  /// Waiting for the local user to approve a no-code request.
  bool awaiting_approval = false;
  uint32_t access_request_id = 0;
  uint64_t connected_ms = 0;
  uint64_t last_seen_ms = 0;
  uint64_t last_ping_ms = 0;
  uint64_t frames_sent = 0;
  /// Sequence on the wire (one per frame sent to this session).
  uint32_t out_seq = 0;
  /// Capture sequence this session has been brought up to. -1 means it has no
  /// usable base image yet.
  int64_t last_capture_seq = -1;
  bool has_image = false;
  /// Cursor state last sent to this session. 0xffffffff forces a shape send.
  uint32_t cursor_serial = 0xffffffffu;
  int cursor_x = INT32_MIN;
  int cursor_y = INT32_MIN;
  bool cursor_visible = false;
  bool cursor_has_shape = false;
  /// Transport-independent channel. The registry owns this reference; the
  /// controller/transport handlers do not keep a stronger one.
  std::shared_ptr<session_transport> transport;
  /// Keeps the session state machine alive while the transport handlers are
  /// installed. Set by session_controller::start().
  std::shared_ptr<session_controller> controller;
};

/// Tracks every live WebSocket session. The registry owns the records, not the
/// connections: libuvcpp owns those and recycles them on close. Callers hold a
/// shared_ptr so a record cannot disappear between a lookup and a send.
class session_registry {
 public:
  std::shared_ptr<session_info> add(
      std::shared_ptr<session_transport> transport);
  void remove(uint32_t id);
  std::shared_ptr<session_info> find(uint32_t id);
  std::shared_ptr<session_info> find_by_public_id(const std::string& public_id);
  std::vector<session_info> snapshot() const;
  std::size_t size() const;
  bool has_authenticated() const;
  /// Authentication is written by the session thread and read by the
  /// supervisor thread through snapshot(); take the registry lock so the
  /// flag is actually visible before the 5 s hello timeout is evaluated.
  void set_authenticated(const std::shared_ptr<session_info>& s);
  void set_awaiting_approval(const std::shared_ptr<session_info>& s,
                             bool awaiting);
  void touch(session_info& s);

 private:
  mutable std::mutex mu_;
  std::vector<std::shared_ptr<session_info>> sessions_;
  uint32_t next_id_ = 1;
};

}  // namespace molink
