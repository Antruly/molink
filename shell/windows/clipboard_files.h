// Clipboard *files*: the part that is not Win32.
//
// The Windows shell owns the clipboard calls (CF_HDROP reads/writes) and the
// byte I/O; this header holds the rules shared with the Web client and the
// host, so Linux unit tests can drive them without a display or a clipboard:
//
//   - which announced files are received: 8 files, 1 TiB each, 4 TiB total
//     (`backend/src/session_controller.cpp` `kClipboardPullMax*`), with the
//     remainder reported as `omitted`;
//   - which local files are offered to the host: 8 files / 2 GiB total
//     (`backend/src/clipboard_lazy.h` `kMaxLazy*`);
//   - the 256 KiB range grammar and the per-range validation;
//   - the retry budget for one range;
//   - the safe on-disk staging directory;
//   - the bounded echo table that stops a file this machine just received
//     from being offered straight back to the host.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace molink::clipboard_files {

/// The host caps one `clipboard-sync.files` list at this count, per-file size
/// and total size. Anything beyond is counted in `omitted`.
inline constexpr std::size_t max_files = 8;
inline constexpr std::uint64_t max_file_bytes = 1ull << 40;   // 1 TiB
inline constexpr std::uint64_t max_total_bytes = 4ull << 40;  // 4 TiB

/// A delayed-rendering offer (`clipboard-offer`) is bounded by the host's
/// `clipboard_lazy` validator; exceeding it makes the host reject the whole
/// selection, so the native side trims it first.
inline constexpr std::uint64_t offer_max_total_bytes = 2ull << 30;  // 2 GiB

/// One `clipboard-pull` range. The host clamps to the same size.
inline constexpr std::uint64_t chunk_bytes = 256ull << 10;

/// No reply to a range request for this long counts as a failed attempt.
inline constexpr std::uint64_t range_timeout_ms = 30'000;

/// One request plus two retries. Status 2 (clipboard changed) is terminal and
/// never retried.
inline constexpr int max_attempts = 3;

/// Echo suppression window and bound, matching `ClipboardEcho`.
inline constexpr std::uint64_t echo_ttl_ms = 2ull * 60 * 1000;
inline constexpr std::size_t echo_max_entries = 64;

/// One entry of `clipboard-sync.files` (or of a local CF_HDROP selection).
/// `path` is the host's own path; the pull path never reads it.
struct entry {
  std::uint32_t id = 0;
  std::string name;
  std::uint64_t size = 0;
  std::string path;
};

struct plan {
  std::vector<entry> files;
  /// Entries left out: invalid metadata plus everything past a limit.
  std::size_t omitted = 0;
  std::uint64_t bytes = 0;
};

/// A safe single file name: no path separators (`/`, `\`, `:`), no control
/// characters, no `* ? " < > |`, leading/trailing spaces and dots trimmed,
/// at most 180 UTF-8 bytes. Empty when nothing safe remains. Mirrors the
/// host's `clipboard_stage::sanitize_name` so a name can never choose the
/// destination path.
std::string sanitize_name(std::string_view raw);

/// Makes `name` unique among `taken` case-insensitively (`a.txt`,
/// `a (1).txt`), adding the result to `taken`.
std::string unique_name(std::string_view name, std::vector<std::string>& taken);

/// Keep the first files that fit the count/per-file/total budget. Invalid
/// entries are counted in `omitted`. Once a limit is reached the rest are
/// omitted too (the Web's `planPullFilesWithLimits` has the same prefix
/// behaviour, so one huge file does not hide the small ones before it).
plan plan_entries(const std::vector<entry>& raw, std::size_t max_count,
                  std::uint64_t max_file, std::uint64_t max_total);

/// Inbound pulls: the host's own 8 / 1 TiB / 4 TiB caps.
plan plan_pull_entries(const std::vector<entry>& raw);

/// Outbound delayed offer: 8 files / 2 GiB total (per-file cap is the total).
plan plan_offer_entries(const std::vector<entry>& raw);

/// Bytes to ask for at `offset`; zero means the file is complete.
std::uint64_t chunk_length(std::uint64_t size, std::uint64_t offset);

enum class chunk_state {
  ok,
  gone,        ///< status 2: the host's clipboard changed; terminal
  failed,      ///< status 1: read error; retryable
  incomplete,  ///< wrong offset or short/over-long reply; terminal
};

/// Validates one 0x18 reply before its bytes are written. A short read is
/// never treated as end-of-file: the announced size owns the file.
chunk_state check_chunk(std::uint32_t status, std::uint64_t chunk_offset,
                        std::uint64_t expected_offset,
                        std::uint64_t expected_length,
                        std::uint64_t byte_length);

/// Retry state for one range. `failed()` records an attempt and says whether
/// another request may be sent; `succeeded()` resets the budget once bytes
/// arrive.
struct range_retry {
  int attempts = 0;
  bool failed() {
    ++attempts;
    return attempts < max_attempts;
  }
  void succeeded() { attempts = 0; }
  bool exhausted() const { return attempts >= max_attempts; }
};

/// `clipboard/<session>/<sequence>` using `/`. The shell joins it below
/// `%LOCALAPPDATA%\moLink`. `session` is reduced to [A-Za-z0-9._-] so it can
/// never name a parent directory; `sequence` is the 1-based receive batch.
std::string staging_relative_directory(std::string_view session,
                                       std::uint32_t sequence);

/// Echo key for a path: lower-cased, `\` treated as `/`, repeated separators
/// collapsed, trailing separator removed (Windows is case-insensitive).
std::string normalize_path_key(std::string_view path);

/// Bounded echo suppression for automatic clipboard files. It remembers both
/// local paths this machine wrote from the host and `name+size` descriptors it
/// offered to the host, so neither direction bounces its own selection back.
class file_echo {
 public:
  void remember_path(std::string_view path, std::uint64_t now_ms);
  void remember_descriptor(std::string_view name, std::uint64_t size,
                           std::uint64_t now_ms);
  /// True when this path/descriptor is inside the two-minute echo window.
  bool suppress_path(std::string_view path, std::uint64_t now_ms);
  bool suppress_descriptor(std::string_view name, std::uint64_t size,
                           std::uint64_t now_ms);
  std::size_t size() const { return entries_.size(); }
  void clear() { entries_.clear(); }

 private:
  struct item {
    std::string key;
    std::uint64_t at = 0;
  };
  void remember(std::string key, std::uint64_t now_ms);
  void prune(std::uint64_t now_ms);
  bool has(std::string_view key);

  std::deque<item> entries_;
};

}  // namespace molink::clipboard_files
