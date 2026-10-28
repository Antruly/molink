#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace molink {

/// Close codes used by the session layer. WebSocket transports pass them
/// through (1000/1001/1008); stream transports map them to their own close or
/// reset semantics.
namespace transport_close {
inline constexpr int normal = 1000;
inline constexpr int going_away = 1001;
inline constexpr int policy = 1008;
}  // namespace transport_close

/// Logical traffic classes. A transport implementation may map these classes
/// to different physical paths (for example a WebRTC video track and a QUIC
/// file stream). The legacy text/binary methods remain the compatibility API.
enum class transport_channel : uint8_t {
  control,
  video,
  audio,
  file,
};

/// Physical capabilities advertised by a transport. The defaults describe the
/// legacy single reliable path and let newer transports opt into independent
/// channel delivery incrementally.
struct transport_capabilities {
  bool independent_control = false;
  bool independent_video = false;
  bool independent_audio = false;
  bool independent_file = false;
  bool unreliable_video = false;
};

/// Transport-independent view of one authenticated session channel.
///
/// The session controller only needs text control messages, binary MOLK
/// packets, peer identity and a way to close. WebSocket is the first
/// implementation; a TCP/QUIC transport can implement the same surface and
/// reuse the controller unchanged.
///
/// Threading: all methods are called on the app loop thread that owns the
/// underlying socket. `set_handlers` installs callbacks that are also invoked
/// on that thread.
class session_transport {
 public:
  using text_handler = std::function<void(const std::string&)>;
  using binary_handler = std::function<void(const uint8_t*, std::size_t)>;
  using close_handler = std::function<void(int code, const std::string& reason)>;

  virtual ~session_transport() = default;

  /// The wire protocol carrying this session ("websocket", "quic", "udp",
  /// "tcp"). This is what the welcome reports as `transport` and what the UI
  /// labels next to the path.
  virtual const char* name() const = 0;
  /// How the bytes reach the peer, independent of the wire protocol:
  /// "direct" (the peer connected to this core), "p2p" (ICE-selected UDP
  /// between two cores) or "relay" (a directory relays the session). A relay
  /// can carry any protocol, so this must not be inferred from name().
  virtual const char* path() const { return "direct"; }
  virtual std::string peer_ip() const = 0;
  virtual unsigned int peer_port() const = 0;
  virtual transport_capabilities capabilities() const {
    return transport_capabilities();
  }

  virtual bool is_open() const = 0;
  virtual bool send_text(const std::string& text) = 0;
  virtual bool send_binary(const uint8_t* data, std::size_t len) = 0;

  /// Sends a packet on a logical channel. Legacy transports intentionally
  /// default to the existing binary path; specialized transports can override
  /// this method to route video, files and control independently.
  virtual bool send_binary(transport_channel channel, const uint8_t* data,
                           std::size_t len) {
    (void)channel;
    return send_binary(data, len);
  }

  /// Sends a control message. This overload gives future transports an
  /// explicit control boundary while preserving the old text API.
  virtual bool send_text(transport_channel channel, const std::string& text) {
    (void)channel;
    return send_text(text);
  }

  virtual void close(int code, const std::string& reason) = 0;

  /// Installs the inbound handlers. Passing empty handlers is allowed and
  /// detaches the previous ones (used by the controller during teardown).
  virtual void set_handlers(text_handler on_text, binary_handler on_binary,
                            close_handler on_close) = 0;
};

}  // namespace molink
