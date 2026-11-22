#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "clipboard/clipboard.h"

namespace molink {

/// One file the host announced to a viewer in `clipboard-sync.files`.
/// `id` is 1-based and session-scoped; it identifies the file's position in
/// the list the host last published to that session.
struct clipboard_offer_entry {
  uint32_t id = 0;
  std::string name;
  uint64_t size = 0;
};

/// Wire status of a `clipboard_file` (0x18) range reply.
enum class clipboard_pull_status : uint32_t {
  ok = 0,
  /// The announced file exists but could not be opened/read.
  read_error = 1,
  /// The clipboard changed (text replaced files, the count/order changed, or
  /// an RDP-redirected temporary file disappeared). The client must report
  /// this and must not create a half file.
  gone = 2,
};

struct clipboard_pull_result {
  clipboard_pull_status status = clipboard_pull_status::gone;
  uint64_t offset = 0;
  std::vector<uint8_t> data;
};

/// Answer one range request for a host clipboard file.
///
/// @param current  CF_HDROP / text-uri-list entries enumerated *now*. The host
///                 re-enumerates on every pull because RDP redirection replaces
///                 its temporary files.
/// @param offered  The list last announced to this session.
/// @param id       1-based file id from the pull request.
/// @param offset   Byte offset the client already has.
/// @param length   Requested range (already clamped by the caller).
/// @param max_chunk Hard upper bound on the returned bytes.
///
/// A mismatch between @p current and @p offered is status `gone`: the client
/// can no longer trust that id to name the bytes it was told about.
clipboard_pull_result read_clipboard_pull(
    const std::vector<clipboard_file>& current,
    const std::vector<clipboard_offer_entry>& offered, uint32_t id,
    uint64_t offset, uint64_t length, uint64_t max_chunk);

}  // namespace molink
