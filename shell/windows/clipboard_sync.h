// Clipboard text sync: the part that is not Win32.
//
// The Windows shell owns the clipboard calls (AddClipboardFormatListener,
// CF_UNICODETEXT reads/writes); everything that decides *whether* a change
// goes on the wire lives here so Linux unit tests can drive it without a
// display or a clipboard.
//
// The rules mirror `frontend/src/lib/session-connection.ts` and
// `frontend/src/lib/client.ts`:
//
//   - empty text is never sent;
//   - text over the host's 192 KiB limit is skipped (the host drops it too:
//     `session_controller.cpp` silently ignores an oversized clipboard-sync);
//   - text identical to the last value sent is not sent again;
//   - text this client just wrote to the local clipboard because the host
//     pushed it is not echoed back. The Web keeps that value in
//     `lastClipboard`; the file path echo table gives it a two-minute TTL,
//     which is the window used here so an intentional re-copy after two
//     minutes syncs again.
//
// The payload is byte-for-byte what `Client.sendClipboardSync` builds:
// `{"type":"clipboard-sync","text":...,"hash":<fnv-1a>,"chain":""}` with the
// FNV-1a hash taken over UTF-16 code units, exactly like `charCodeAt`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace molink::clipboard {

/// The host's text cap (`session_controller.cpp`, clipboard backends). It is
/// measured in UTF-8 bytes, the same unit the host's `std::string` uses.
inline constexpr std::size_t max_text_bytes = 192 * 1024;

/// How long a text this client wrote for the host is treated as an echo.
inline constexpr std::uint64_t echo_ttl_ms = 2 * 60 * 1000;

/// The echo table's bound, shared with `ClipboardEcho`.
inline constexpr std::size_t echo_max_entries = 64;

/// Why a local clipboard change did not produce a `clipboard-sync`.
enum class skip_reason {
  none,
  disabled,   ///< the session's clipboard switch is off
  empty,      ///< nothing on the local clipboard, or no text format
  too_large,  ///< over `max_text_bytes`; the host would drop it silently
  duplicate,  ///< identical to the last value sent
  echo,       ///< this client just wrote it from the host's clipboard
};

/// The decision for one local clipboard update.
struct decision {
  bool send = false;
  skip_reason reason = skip_reason::none;
  /// UTF-8 payload, valid whenever `reason` is not `disabled` or `empty`.
  std::string text;
  /// UTF-8 length of `text`, the number the log and the host cap use.
  std::size_t bytes = 0;
};

/// UTF-16 (Windows `wchar_t` / JS code units) to UTF-8. Unpaired surrogates
/// become U+FFFD, so the wire payload is always valid JSON text.
std::string utf8_from_utf16(std::u16string_view text);

/// FNV-1a over UTF-16 code units, lower-case hex without padding, exactly
/// `(hash >>> 0).toString(16)` from the Web client.
std::string fnv1a_hex(std::u16string_view text);

/// Escapes UTF-8 for a JSON string body (quotes, backslashes, controls).
std::string json_escape_utf8(std::string_view text);

/// The exact Web payload; key order matches `sendClipboardSync`.
std::string clipboard_sync_json(std::u16string_view text);

/// Dedup / echo state for one session.
class text_sync {
 public:
  void set_enabled(bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }

  /// A local clipboard update (the `WM_CLIPBOARDUPDATE` read).
  decision on_local_text(std::u16string_view text, std::uint64_t now_ms);

  /// The shell is about to write host-pushed text into the local clipboard.
  /// Call before writing so the update the write generates is recognised.
  void on_remote_text(std::u16string_view text, std::uint64_t now_ms);

  /// A send failed (socket closed): allow the same text to go out again.
  void reset_last_sent() { last_sent_key_.clear(); }

  void clear_echo();
  std::size_t echo_count() const { return echoes_.size(); }

 private:
  struct echo_entry {
    std::string key;
    std::uint64_t at = 0;
  };

  static std::string content_key(std::string_view utf8,
                                 std::u16string_view text);
  void prune(std::uint64_t now_ms);
  bool has_echo(const std::string& key);

  bool enabled_ = true;
  std::string last_sent_key_;
  std::deque<echo_entry> echoes_;
};

}  // namespace molink::clipboard
