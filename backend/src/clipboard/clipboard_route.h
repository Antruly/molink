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
  /// When the viewer last renewed `video-push on` (monotonic ms). 0 means the
  /// viewer has never named itself as the visible tab, or named another one
  /// since. The hint is a lease: it expires after kClipboardForegroundTtlMs,
  /// so a viewer that stops renewing (a crashed tab, a minimized shell that
  /// forgot to say so) cannot stay the clipboard target forever.
  uint64_t foreground_ms = 0;
  /// Last input message from this viewer (0 = never).
  uint64_t last_input_ms = 0;
  /// When the viewer authenticated; the tie-breaker when nobody has interacted.
  uint64_t connected_ms = 0;
};

enum class clipboard_route_reason {
  none,      ///< no connected, authenticated session
  foreground,///< at least one viewer renewed a fresh foreground hint
  input,     ///< the most recent viewer to send input is the active one
  sole,      ///< only one viewer is connected
  broadcast, ///< several viewers; the host-wide opt-in allows fan-out
  no_target, ///< several viewers and nothing names one; deliver nowhere
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

/// How long a `video-push on` foreground hint stays valid. The viewer renews
/// it while it is being watched; once the lease expires the hint is ignored
/// even if that client is still connected (a minimized shell, a crashed tab).
inline constexpr uint64_t kClipboardForegroundTtlMs = 15 * 1000;

/// Pick the destination(s) for a host clipboard change.
///
/// The rules in order:
///  1. A viewer with a fresh foreground lease is the active tab. When several
///     leases are fresh (two windows), the one that interacted most recently
///     wins; this is still a single-session route.
///  2. Otherwise the viewer that sent input most recently, within the window.
///  3. Otherwise, a lone viewer is obviously the one being watched.
///  4. Otherwise, nothing is delivered (`reason=no_target`). Broadcasting to
///     every viewer used to leak one clipboard into unrelated sessions, so a
///     host must opt in with `allow_broadcast`; only then is every
///     authenticated viewer a target (`reason=broadcast`).
clipboard_route select_clipboard_targets(
    const std::vector<clipboard_route_session>& sessions, uint64_t now_ms,
    uint64_t input_window_ms = kClipboardInputActiveMs,
    bool allow_broadcast = false);

/// True when a fresh foreground lease still names this viewer as on screen.
bool clipboard_foreground_active(const clipboard_route_session& session,
                                 uint64_t now_ms);
bool clipboard_foreground_active(uint64_t foreground_ms, uint64_t now_ms);

/// Absolute host paths are only safe to hand to a peer on this same machine
/// (the local core's page and the native shell). A remote viewer gets ids and
/// names, never `C:\Users\alice\...`.
bool clipboard_peer_may_see_paths(const std::string& peer_ip);

/// Decide whether a polling watcher reports a value to the host.
///
/// `new_change` comes from the platform's own counter (Windows sequence
/// number, NSPasteboard changeCount, an XFixes selection event). A new change
/// is always reported, even when the content is identical: the second copy of
/// the same password is a real copy. Content equality is only the fallback
/// for a platform with no counter, where a changed value is all we can see.
bool clipboard_watcher_should_report(bool new_change,
                                     const std::string& content,
                                     const std::string& last_content);

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
