// Pure file-panel rules shared by the native shell and its Linux tests.
//
// This translation unit deliberately has no Win32 dependency: the path
// grammar, the multi-selection state machine and the task aggregation are
// copied from the Web's `frontend/src/lib/file-transfer.ts` so both clients
// make the same decisions. The Win32 calls (drives, FindFirstFileW, D2D) stay
// in ui_kit_demo.cpp / session_client.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace molink::files {

/// Host's per-file ceiling (`file_transfer::kMaxFileBytes`).
constexpr uint64_t kMaxFileBytes = 8ull * 1024 * 1024 * 1024;
/// Host's concurrent upload/download slots.
constexpr std::size_t kMaxActiveTransfers = 8;

// ---------------------------------------------------------------------------
// Paths (Web `file-transfer.ts`)
// ---------------------------------------------------------------------------

/// Slash-separated, no leading `./`, no duplicate or trailing separators.
std::string normalize_relative_path(const std::string& raw);

/// Lexically normalise a path that may be absolute, keeping its drive/UNC
/// prefix. `..` cannot climb above the root. Output always uses `/`.
std::string normalize_remote_path(const std::string& raw);

/// Resolves what the user typed into a path box. Empty means the host's home
/// (`root`); `~` expands against `root`; absolute paths are normalised as-is;
/// relative paths resolve against the displayed folder (`base`).
std::string resolve_remote_path(const std::string& text,
                                const std::string& base,
                                const std::string& root);

/// Joins a name onto a displayed folder using that folder's separator.
std::string join_path(const std::string& base, const std::string& name);

/// The parent shown by the up button. A volume/share root returns "".
std::string parent_path(const std::string& path);

struct path_crumb {
  std::string name;
  std::string path;
};

/// Clickable breadcrumb prefixes; the roots view ("") has no crumbs.
std::vector<path_crumb> path_crumbs(const std::string& path);

enum class path_flavor_kind { unknown, windows, posix };
path_flavor_kind path_flavor(const std::string& path);

/// The Web's folder-walk budget: `FileTransferPanel.tsx` stops at 20000
/// entries and refuses to descend past 64 levels. Keeping the same numbers
/// means a folder the page sends is exactly the folder the native shell sends.
constexpr std::size_t kMaxWalkEntries = 20000;
constexpr int kMaxWalkDepth = 64;

/// One directory child as returned by a source lister. `name` is one segment.
struct walk_entry {
  std::string name;
  uint64_t size = 0;
  bool dir = false;
  /// Reparse point / symlink: list it as a folder, but never descend (a loop
  /// would recurse forever). The Web's `walkFs` does the same.
  bool link = false;
};

/// Lists one directory. Returns false and may fill `error` when it cannot be
/// read; the walk counts that folder as skipped instead of aborting.
using walk_lister = std::function<bool(const std::string& path,
                                       std::vector<walk_entry>& children,
                                       std::string* error)>;

/// One selection root: the source path handed to the lister plus the relative
/// path it owns at the destination.
struct walk_root {
  std::string path;
  std::string rel;
  bool dir = true;
  uint64_t size = 0;
};

struct walk_file {
  std::string path;
  std::string rel;
  uint64_t size = 0;
};

struct walk_result {
  std::vector<walk_file> files;
  /// Every folder, including empty ones. Sorted parents-first.
  std::vector<std::string> dirs;
  /// Folders with no file anywhere beneath them: the only ones an upload has
  /// to create explicitly (an `offer` creates the parents of its own path).
  std::vector<std::string> empty_dirs;
  /// Unsafe names and unreadable folders.
  int skipped = 0;
  /// The entry/depth budget cut the walk short.
  bool truncated = false;
};

struct walk_limits {
  std::size_t max_entries = kMaxWalkEntries;
  int max_depth = kMaxWalkDepth;
};

/// Expands selection roots into files plus directories without touching the
/// filesystem itself. The caller supplies the lister (FindFirstFileW for an
/// upload, a fake tree in the tests), so the recursion, deduplication, empty
/// folder detection and limits are identical on both sides.
walk_result walk_tree(const std::vector<walk_root>& roots,
                      const walk_lister& list,
                      const walk_limits& limits = walk_limits());

/// Web `relativeUnder`: the slash-separated path below `root`, or "" when
/// `path` is not strictly inside it.
std::string relative_within(const std::string& root, const std::string& path);

/// Converts a slash-separated relative path to `separator` (`photos/a.jpg` ->
/// `photos\a.jpg` on Windows). No normalisation is applied.
std::string to_native_path(const std::string& relative, char separator);

/// True when `raw` can be sent to the host as an upload name: non-empty,
/// relative, at most 64 segments, no drive/UNC/absolute prefix, no `.`/`..`
/// and no Windows reserved device name segment.
bool is_safe_relative_path(const std::string& raw);

/// Sanitises a peer-supplied relative path for a local write. `..`, `.` and
/// empty segments are dropped so the result can never climb out of the
/// download root; Windows-illegal characters become `_`, trailing spaces and
/// dots are trimmed, and reserved names are prefixed. An empty result becomes
/// `{fallback}`.
std::vector<std::string> safe_target_segments(const std::string& raw,
                                              const std::string& fallback);

// ---------------------------------------------------------------------------
// Multi-selection (Ctrl / Shift / select-all)
// ---------------------------------------------------------------------------

/// Index-keyed selection shared by the local and remote lists. Plain click
/// selects one, Ctrl toggles one, Shift extends from the anchor (adding to the
/// set when Ctrl is held, replacing it otherwise, like a file manager).
class selection_model {
 public:
  void click(std::size_t index, bool ctrl, bool shift, std::size_t count);
  void toggle(std::size_t index);
  void set(std::size_t index, bool selected);
  bool contains(std::size_t index) const;
  void select_all(std::size_t count);
  void clear();
  std::size_t size() const { return selected_.size(); }
  bool empty() const { return selected_.empty(); }
  bool all_selected(std::size_t count) const {
    return count > 0 && selected_.size() == count;
  }
  const std::unordered_set<std::size_t>& indexes() const { return selected_; }

 private:
  std::unordered_set<std::size_t> selected_;
  std::size_t anchor_ = 0;
  bool has_anchor_ = false;
};

// ---------------------------------------------------------------------------
// Task aggregation (Web `aggregateProgress` / `taskStatusFromChildren`)
// ---------------------------------------------------------------------------

enum class transfer_status { queued, running, done, error, cancelled };

struct transfer_item {
  uint64_t size = 0;
  uint64_t progress = 0;
  transfer_status status = transfer_status::queued;
  /// Refused before a transfer started (a folder in this batch, an oversized
  /// file). Skipped items count towards `total` but carry no bytes/status.
  bool skipped = false;
};

struct transfer_summary {
  int total = 0;        ///< children + skipped
  int done_files = 0;
  int failed_files = 0;
  int cancelled_files = 0;
  int skipped = 0;
  uint64_t total_bytes = 0;
  uint64_t done_bytes = 0;
  int percent = 0;
  transfer_status status = transfer_status::done;
};

transfer_summary summarize_transfers(const std::vector<transfer_item>& items);

// ---------------------------------------------------------------------------
// Button block reasons (Web `uploadBlockReason` / `downloadBlockReason`)
// ---------------------------------------------------------------------------

enum class transfer_block {
  none,
  no_selection,
  no_target,
  no_source,
  busy,
  unsupported,
};

transfer_block upload_block_reason(std::size_t selected, bool has_source,
                                   bool has_target, bool busy);
transfer_block download_block_reason(std::size_t selected, bool busy);

/// True when a path is absolutely addressed (POSIX, drive letter or UNC).
bool is_absolute_path(const std::string& path);

}  // namespace molink::files
