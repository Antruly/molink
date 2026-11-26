// Wire protocol between molink-core's terminal service and
// molink-helper.exe on Windows.
//
// The helper already serves the `\\.\pipe\molink-secure` pipe for secure
// desktop capture and input. The terminal reuses that pipe instead of opening
// a second one: a `term-open` request upgrades the connection from the
// existing one-command/one-reply style into a byte-stream session.
//
// Only the constants live here, so the helper (which includes backend/src via
// the molink-diag target) and the core cannot drift apart. The helper runs as
// the service account, so it is the only process that can call
// WTSQueryUserToken and start the shell as the console user; the core merely
// forwards bytes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace molink {
namespace terminal_bridge {

/// The helper's existing secure-desktop pipe, shared with capture/input.
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\molink-secure";

/// Request line: "term-open <cols> <rows>[ <shell>]". Everything after the
/// two numbers (minus the single separating space) is the UTF-8 command line;
/// an empty command asks the helper for the user's %COMSPEC%.
inline constexpr char kOpenPrefix[] = "term-open";

/// Success reply (one message, then frames): "term-ok pid=<n> session=<n>
/// user=<rest of line>", failure reply: "term-error reason=<code>
/// message=<rest of line>".
inline constexpr char kOkPrefix[] = "term-ok ";
inline constexpr char kErrorPrefix[] = "term-error ";

/// Largest payload in one pipe message. Keeps a single frame well below the
/// core's 64 KiB read buffer and below the helper's 64 KiB command buffer.
inline constexpr std::size_t kMaxFramePayload = 32u * 1024u;

/// core -> helper frame kinds.
enum class to_helper : std::uint8_t {
  input = 0x01,
  resize = 0x02,
  close = 0x03,
};

/// helper -> core frame kinds.
enum class from_helper : std::uint8_t {
  output = 0x01,
  exit = 0x02,
};

/// Encoded sizes. Both are little-endian on Windows.
inline constexpr std::size_t kResizePayload = 4 + 4;  // u32 cols, u32 rows
inline constexpr std::size_t kExitPayload = 4;        // int32 exit code

}  // namespace terminal_bridge
}  // namespace molink
