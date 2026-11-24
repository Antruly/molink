#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace molink {

/// Who put the current value on the local clipboard.
///
/// The host only needs one distinction: a value this process wrote (an echo of
/// a viewer's copy) must not travel back out, while anything else -- another
/// remote-control tool, an inner RDP session, a local application, or an owner
/// the platform cannot name -- is a normal local copy and must be announced.
/// `unknown` is therefore *accepted*; it is not a reason to drop the change.
enum class clipboard_owner { unknown, self, external };

/// Wire/log spelling: "self", "external", "unknown".
const char* clipboard_owner_name(clipboard_owner owner);

/// One connected session, as the clipboard router sees it. Kept free of
/// session_info so the selection rules can be tested without a transport.
struct clipboard_route_session {
  uint32_t id = 0;
  bool authenticated = false;
  bool connected = true;
  /// The viewer said its tab/window is the one on screen (`video-push on`).
  bool foreground = false;
  /// Last input message from this viewer (0 = never).
  uint64_t last_input_ms = 0;
  /// When the viewer authenticated; the tie-breaker when nobody has interacted.
  uint64_t connected_ms = 0;
};

enum class clipboard_route_reason {
  none,       ///< no connected, authenticated session
  foreground, ///< at least one viewer marked itself as the visible tab
  input,      ///< the most recent viewer to send input is the active one
  sole,       ///< only one viewer is connected
  broadcast,  ///< several viewers and no activity signal (legacy clients)
};

const char* clipboard_route_reason_name(clipboard_route_reason reason);

struct clipboard_route {
  /// Sessions that should receive a host clipboard change.
  std::vector<uint32_t> targets;
  /// The session considered active, or 0 when none is connected.
  uint32_t active = 0;
  clipboard_route_reason reason = clipboard_route_reason::none;
};

/// How long a viewer's input keeps it "active" when no tab is foreground.
inline constexpr uint64_t kClipboardInputActiveMs = 2 * 60 * 1000;

/// Pick the destination(s) for a host clipboard change.
///
/// The rules in order:
///  1. A viewer that set `video-push on` is the active tab. When several are
///     visible at once (two windows), the one that interacted most recently
///     wins; this is still a single-session route.
///  2. Otherwise the viewer that sent input most recently, within the window.
///  3. Otherwise, a lone viewer is obviously the one being watched.
///  4. Otherwise every authenticated viewer, which is the old broadcast
///     behaviour. This is the compatibility path for clients that never send
///     `video-push`; the diagnostic records `reason=broadcast` so the caller
///     can tell an intentional fallback from a routed delivery.
clipboard_route select_clipboard_targets(
    const std::vector<clipboard_route_session>& sessions, uint64_t now_ms,
    uint64_t input_window_ms = kClipboardInputActiveMs);

/// Echo-suppression key for text. The hash alone can collide, so the byte
/// length is included. The nested-loop `chain` is deliberately not part of the
/// key: a viewer's own poll sends an empty chain, so including it would make
/// its legitimate echo look like a new copy. The chain is checked separately
/// before the text is applied.
std::string clipboard_text_key(const std::string& hash, std::size_t bytes);

/// Echo-suppression key for a file list signature.
std::string clipboard_files_key(const std::string& signature);

/// Which direction a remembered key travelled.
enum class clipboard_echo_direction { delivered, received };

const char* clipboard_echo_direction_name(clipboard_echo_direction direction);

/// Per-session echo table.
///
/// A host-wide set of hashes was not enough once two viewers were connected:
/// viewer A's value was remembered globally, so viewer B's genuine copy of the
/// same text was dropped, and a value delivered to A could be treated as B's
/// echo. Entries are therefore keyed by `(session_id, key)`.
///
/// `delivered` records text/files this host sent to a viewer; when that viewer
/// sends the same value back it is an echo. `received` records a value taken
/// from a viewer's clipboard; the watcher must not announce it as a fresh host
/// copy, but other viewers may still be offered it.
///
/// Thread-safe: clipboard watchers run on a backend thread, while sends are
/// posted to the app loop.
class clipboard_echo_table {
 public:
  explicit clipboard_echo_table(uint64_t ttl_ms = 2 * 60 * 1000,
                                std::size_t max_entries = 256);

  void remember(uint32_t session_id, std::string key,
                clipboard_echo_direction direction, uint64_t now_ms);

  /// True when this session already saw/sent `key` within the TTL. The entry
  /// stays until it expires or is evicted, so repeated echoes stay suppressed.
  bool suppress(uint32_t session_id, const std::string& key, uint64_t now_ms);

  /// The session disconnected: its entries can never match again.
  void forget_session(uint32_t session_id);

  void clear();

  std::size_t size() const;

  uint64_t ttl_ms() const { return ttl_ms_; }
  std::size_t max_entries() const { return max_entries_; }

 private:
  struct entry {
    uint32_t session_id = 0;
    std::string key;
    clipboard_echo_direction direction = clipboard_echo_direction::delivered;
    uint64_t at = 0;
  };

  void prune_locked(uint64_t now_ms);

  uint64_t ttl_ms_;
  std::size_t max_entries_;
  mutable std::mutex mutex_;
  std::deque<entry> entries_;
};

}  // namespace molink
