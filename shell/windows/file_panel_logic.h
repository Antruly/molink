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
