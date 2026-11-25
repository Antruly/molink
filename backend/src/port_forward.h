#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace molink {

/// One local TCP forward: `listen_host:listen_port` on this core is tunnelled
/// through the session to `target_host:target_port` as seen by the peer.
///
/// The first block is configuration and is persisted in the line-oriented
/// settings file (`port_mapping=` lines). The second block is live state filled
/// in by the manager; it is never written to disk.
struct port_mapping {
  uint64_t id = 0;
  std::string listen_host = "127.0.0.1";
  uint16_t listen_port = 0;  ///< 0 = let the system pick; updated after bind.
  std::string target_host = "127.0.0.1";
  uint16_t target_port = 0;
  bool enabled = true;

  // Runtime state.
  bool listening = false;
  uint64_t bytes_in = 0;   ///< local client -> target
  uint64_t bytes_out = 0;  ///< target -> local client
  uint32_t active_streams = 0;
  std::string last_error;
};

/// A decoded `forward-*` control message.
struct port_forward_message {
  std::string type;
  uint16_t stream = 0;
  std::string host;
  uint16_t port = 0;
  std::string error;
};

/// True for the four message types this module owns.
bool is_port_forward_message_type(const std::string& type);

/// Parses a flat JSON control message. Unknown fields are ignored; a malformed
/// document, a missing `type`, or a bad `stream`/`port` fills @p error.
bool decode_port_forward_message(const std::string& text,
                                 port_forward_message& out,
                                 std::string& error);

std::string encode_forward_open(uint16_t stream, const std::string& host,
                                uint16_t port);
std::string encode_forward_open_ok(uint16_t stream);
std::string encode_forward_open_fail(uint16_t stream,
                                     const std::string& error);
std::string encode_forward_close(uint16_t stream);

/// Payload of a `forward_data` MOLK packet: a 16-bit stream id in network
/// order followed by the raw bytes. The MOLK header (and its type byte) is
/// added by the session adapter, so this stays independent of libuvcpp.
std::vector<uint8_t> encode_port_forward_payload(uint16_t stream,
                                                 const uint8_t* data,
                                                 std::size_t len);
bool decode_port_forward_payload(const uint8_t* data, std::size_t len,
                                 uint16_t& stream, const uint8_t*& payload,
                                 std::size_t& payload_len);

/// Persistence for the configuration half of the mappings. Implementations
/// must be safe to call from the manager's API thread.
class port_forward_store {
 public:
  virtual ~port_forward_store() = default;
  virtual bool load(std::vector<port_mapping>& mappings,
                    std::string& error) = 0;
  virtual bool save(const std::vector<port_mapping>& mappings,
                    std::string& error) = 0;
};

/// Reads and writes `port_mapping=` lines in the core's settings file. Other
/// lines (including every ordinary setting) are preserved verbatim, so this
/// can share the file with save_settings().
std::unique_ptr<port_forward_store> make_settings_port_forward_store(
    std::string settings_path);

/// The live session leg. The manager sends opens/data through it and the
/// session adapter feeds inbound messages back into on_control/on_data.
///
/// Implementations may be called from the manager's worker threads, so
/// send_text/send_data must be thread-safe. `is_open()` is a cheap liveness
/// check, not a lock.
class port_forward_link {
 public:
  virtual ~port_forward_link() = default;
  virtual bool is_open() const = 0;
  virtual bool send_text(const std::string& text) = 0;
  virtual bool send_data(uint16_t stream, const uint8_t* data,
                         std::size_t len) = 0;
  /// Called by the owner when the session goes away. Implementations whose
  /// is_open() would otherwise touch the (loop-affine) transport should latch
  /// a flag here, because relay workers call is_open() from their own threads.
  virtual void mark_closed() {}
};

/// Controller-side (viewer-side) mapping manager.
///
/// Listeners are created per enabled mapping and own their accept thread, so
/// they survive a session drop. When a local TCP connection arrives:
///   * with a live link, a stream id is allocated and `forward-open` is sent;
///   * with no link, the connection is refused immediately (logged as
///     `portmap.error ... err=no_session`). Nothing is queued: an RDP/SSH
///     client retries on its own, and a queued connection would grow stale.
///
/// detach_link() closes every live stream but deliberately leaves the
/// listeners bound; attach_link() on the reconnected session makes the next
/// local connection work again without the user touching the mapping.
class port_forward_manager {
 public:
  using enabled_fn = std::function<bool()>;

  /// @p enabled is read live (the host capability switch). An empty callback
  /// means "disabled".
  port_forward_manager(std::unique_ptr<port_forward_store> store,
                       enabled_fn enabled = {});
  ~port_forward_manager();

  port_forward_manager(const port_forward_manager&) = delete;
  port_forward_manager& operator=(const port_forward_manager&) = delete;

  /// Creates a mapping and starts its listener. On success @p mapping.id,
  /// `listen_port` (the OS-assigned port when 0 was requested) and runtime
  /// fields are filled in. Returns false with @p error for an invalid mapping,
  /// when the capability is off, when the listen address cannot be bound, or
  /// when the settings file cannot be written; a failed add leaves no row.
  bool add(port_mapping& mapping, std::string& error);
  /// Replaces the configuration of an existing mapping. Changing the endpoint
  /// or enabled flag restarts the listener and closes that mapping's streams.
  bool update(const port_mapping& mapping, std::string& error);
  bool remove(uint64_t id);
  bool remove(uint64_t id, std::string& error);
  bool set_enabled(uint64_t id, bool enabled, std::string& error);
  /// Reads the capability switch again. Disabling it stops every listener;
  /// enabling it starts the persisted enabled mappings again. Returns how many
  /// listeners are running afterwards.
  std::size_t refresh();

  std::vector<port_mapping> mappings() const;
  bool get(uint64_t id, port_mapping& out) const;

  // ---- session attachment ------------------------------------------------
  void attach_link(std::shared_ptr<port_forward_link> link);
  void detach_link();
  bool linked() const;

  /// Inbound `forward-open-ok` / `forward-open-fail` / `forward-close`.
  void on_control(const std::string& text);
  /// Raw bytes for one stream (payload without the MOLK header).
  void on_data(uint16_t stream, const uint8_t* data, std::size_t len);

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

/// Target-side (controlled-host) handler for one authenticated session.
///
/// It connects `host:port` when the controller asks, replies ok/fail, and
/// relays bytes in both directions. Every target connection is a normal
/// outbound TCP socket owned by this core; the host capability switch
/// (`port_forward_enabled`) is checked for every open and refused as
/// `port_forward_disabled` when off.
class port_forward_host {
 public:
  using enabled_fn = std::function<bool()>;

  port_forward_host(std::shared_ptr<port_forward_link> link,
                    enabled_fn enabled = {});
  ~port_forward_host();

  port_forward_host(const port_forward_host&) = delete;
  port_forward_host& operator=(const port_forward_host&) = delete;

  /// Inbound `forward-open` / `forward-close`.
  void on_control(const std::string& text);
  /// Raw bytes for one stream.
  void on_data(uint16_t stream, const uint8_t* data, std::size_t len);

  std::size_t active_streams() const;
  /// Closes every target connection and waits for the workers to exit. Safe to
  /// call more than once and from the session's close handler.
  void stop();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace molink
