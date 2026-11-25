#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "terminal/pty.h"

namespace molink {

/// Fixed-size circular buffer of terminal output bytes.
///
/// `seq()` is the total number of bytes ever appended, so it is also the
/// sequence number of the next byte. `oldest_seq()` is the sequence number of
/// the first byte still buffered (equal to `seq()` when empty). A viewer
/// attaches with the last byte it saw and receives everything from there; if
/// that byte has already been evicted the replay starts at the oldest byte and
/// reports `truncated`.
class terminal_ring_buffer {
 public:
  explicit terminal_ring_buffer(std::size_t capacity = 1024u * 1024u);

  void append(const uint8_t* data, std::size_t len);
  void clear();

  uint64_t seq() const { return seq_; }
  uint64_t oldest_seq() const { return seq_ - size_; }
  std::size_t size() const { return size_; }
  std::size_t capacity() const { return buffer_.size(); }
  bool empty() const { return size_ == 0; }

  /// Copy [from_seq, seq()) into `out`. When `from_seq` is older than the
  /// buffer, copying starts at `oldest_seq()` instead and `truncated` is set.
  /// `start_seq` receives the sequence number of `out.front()` (or `seq()`
  /// when nothing is copied).
  std::vector<uint8_t> replay(uint64_t from_seq,
                              uint64_t& start_seq,
                              bool& truncated) const;

 private:
  std::vector<uint8_t> buffer_;
  /// Index of the oldest byte. The live region is head_ .. head_+size_ (mod
  /// capacity).
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  uint64_t seq_ = 0;
};

class terminal_session;

/// Host-side terminal sessions. A session owns its PTY and output ring. A
/// viewer disconnect reclaims the terminals that viewer opened (see
/// release_viewer); terminals another viewer opened are only detached and keep
/// running until they exit or the idle timeout reaps them.
///
/// Single-threaded by design: open/attach/write/resize/close and poll() are
/// all expected on the core's event-loop thread.
class terminal_service {
 public:
  using output_fn = std::function<void(uint64_t id, uint64_t seq,
                                      const uint8_t* data, std::size_t len)>;
  using exit_fn = std::function<void(uint64_t id, int code)>;

  static constexpr std::size_t kMaxViewerTerminals = 4;
  static constexpr std::size_t kDefaultRingBytes = 1024u * 1024u;

  explicit terminal_service(
      std::size_t ring_bytes = kDefaultRingBytes,
      std::chrono::milliseconds idle_timeout = std::chrono::minutes(30));
  ~terminal_service();

  terminal_service(const terminal_service&) = delete;
  terminal_service& operator=(const terminal_service&) = delete;

  /// A connection-scoped viewer identity. Sessions remember their viewers by
  /// this value, which is how the four-terminal cap is enforced.
  uint64_t register_viewer();
  /// The viewer's connection went away. Detach it from every session and
  /// reclaim (kill and forget) the terminals it created — a viewer that
  /// cannot come back must not leave orphan PTYs behind. Terminals another
  /// viewer created are only detached. Logs `term.reclaim` per session.
  void release_viewer(uint64_t viewer);

  /// Spawn a new terminal and attach `viewer`. Returns 0 and fills `err` when
  /// the spawn failed or the viewer already has four live terminals.
  uint64_t open(uint64_t viewer, const std::string& shell, int cols, int rows,
                std::string& err);

  /// Attach `viewer` and copy the output ring from `from_seq`. `replay_seq`
  /// is the sequence number of the first replayed byte. Returns false when the
  /// id is unknown or the four-terminal cap is hit.
  bool attach(uint64_t viewer, uint64_t id, uint64_t from_seq,
              std::vector<uint8_t>& replay, uint64_t& replay_seq,
              bool& truncated, std::string& err);

  /// Detach one viewer; the process keeps running.
  void detach(uint64_t viewer, uint64_t id);

  bool write(uint64_t id, const uint8_t* data, std::size_t len);
  bool resize(uint64_t id, int cols, int rows);
  /// Kill the process (if it is still alive) and forget the session.
  bool close(uint64_t id, const char* reason);

  /// Drain PTY output, notice child exits and reap idle sessions. Call from a
  /// short-period timer on the event loop.
  void poll();

  /// Listener callbacks receive output and exits for every session; the
  /// WebSocket layer filters them by the ids it attached. Returns a token for
  /// remove_listener().
  uint64_t add_listener(output_fn on_output, exit_fn on_exit);
  void remove_listener(uint64_t token);

  bool has_session(uint64_t id) const;
  /// True when the child has exited; fills `code`.
  bool exit_code(uint64_t id, int& code) const;
  std::size_t session_count() const;

 private:
  struct entry;
  entry* find(uint64_t id);
  const entry* find(uint64_t id) const;
  std::size_t live_count(uint64_t viewer) const;
  void notify_output(uint64_t id, uint64_t seq, const uint8_t* data,
                     std::size_t len);
  void notify_exit(uint64_t id, int code);

  std::size_t ring_bytes_;
  std::chrono::milliseconds idle_timeout_;
  uint64_t next_id_ = 1;
  uint64_t next_viewer_ = 1;
  uint64_t next_listener_ = 1;
  std::map<uint64_t, std::unique_ptr<entry>> sessions_;
  std::map<uint64_t, std::pair<output_fn, exit_fn>> listeners_;
};

}  // namespace molink
