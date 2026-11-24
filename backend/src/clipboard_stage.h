#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace molink {

/// Result of a staging operation. The HTTP layer maps these to status codes;
/// keeping them separate lets the tests exercise the rules without libuvcpp.
enum class stage_error {
  none,
  unavailable,
  bad_request,
  not_found,
  conflict,
  too_large,
  io_error,
  apply_failed,
};

/// A file downloaded from a remote clipboard into the local staging folder.
/// Bytes are written to a hidden `.stage-<id>` file and renamed to
/// `final_path` only when the whole set is applied to the clipboard. The
/// rename keeps the staging folder free of half files and lets the clipboard
/// reference the final paths, which must outlive the apply call.
struct staged_clipboard_entry {
  std::string id;
  std::string name;
  std::string final_path;
  std::string temp_path;
  uint64_t size = 0;
  uint64_t received = 0;
  uint64_t updated_ms = 0;
  /// Selection-relative path (`sub/dir/file`); empty means `name`.
  std::string rel;
  /// A directory the viewer recreated. It has no bytes and is passed to the
  /// clipboard as one of the selection's roots.
  bool is_directory = false;
};

/// Sequential staging for "remote clipboard file -> this machine's clipboard".
///
/// The staged directory is normally `<config>/clipboard-inbox`. A remote page
/// cannot use it: the HTTP routes are loopback-only, and only the local core
/// can write the OS clipboard's file list.
class clipboard_stage {
 public:
  using files_applier = std::function<bool(const std::vector<std::string>&)>;
  using clock_fn = std::function<uint64_t()>;

  /// Largest single file the explicit "取出" path may download. The automatic
  /// fallback uses much smaller caps (64 MiB / 256 MiB total); this only bounds
  /// a selection the user explicitly asked to transfer.
  static constexpr uint64_t kMaxFileBytes = 4ull * 1024 * 1024 * 1024;
  /// A staging entry nobody finished is dropped on the next request after
  /// this long. Applied files are no longer entries and are kept.
  static constexpr uint64_t kEntryTtlMs = 30ull * 60 * 1000;
  /// Copied files stay available to the clipboard for a day. Older files are
  /// swept so a machine that copies large files does not grow without bound.
  static constexpr uint64_t kAppliedTtlMs = 24ull * 60 * 60 * 1000;

  clipboard_stage(std::string directory, files_applier applier,
                  clock_fn clock = {});

  bool available() const { return !directory_.empty(); }

  /// Declares one file or directory. `name` is always a leaf name; `rel`
  /// carries the selection-relative path (`sub/dir/file`) when the remote
  /// copied a folder, and empty means `name`. The bytes go to a private temp
  /// file until apply; a directory is created immediately (including an empty
  /// one) and needs no bytes.
  ///
  /// `batch` (optional) is a lowercase-hex id naming the subdirectory for one
  /// clipboard selection, e.g. `~/.cache/molink/clipboard/<sha1>/`. Isolating
  /// batches keeps a later copy of the same file from colliding with an
  /// earlier one and makes it obvious which files belong together.
  stage_error begin(const std::string& name, uint64_t size, std::string& id,
                    std::string& path, const std::string& batch = {},
                    const std::string& rel = {}, bool is_dir = false);

  /// Appends one contiguous chunk. `offset` must equal the bytes received so
  /// far (`conflict`) and the chunk may not cross the declared size
  /// (`too_large`).
  stage_error append(const std::string& id, uint64_t offset, const char* data,
                     std::size_t length, uint64_t& received);

  /// Verifies that every id is complete, renames the temp files to their final
  /// names, then calls the applier with those paths in the given order. On
  /// success the entries are removed; the files stay because the clipboard
  /// references them.
  stage_error apply(const std::vector<std::string>& ids, std::size_t& count);

  /// Discards an unfinished entry and deletes its temp file. Called when the
  /// clipboard changes mid-download, so a half file does not linger until the
  /// TTL sweep. Unknown ids (already applied or swept) are ignored.
  void abort(const std::string& id);

  /// Drop entries older than kEntryTtlMs (called lazily by every operation).
  void sweep();

  /// Number of staging entries, for tests and diagnostics.
  std::size_t pending() const;

  /// One safe file name: no separators, no control characters, no `.`/`..`,
  /// bounded in bytes. Returns empty when nothing safe remains.
  static std::string sanitize_name(const std::string& raw);
  /// True for a value accepted as a batch directory name: 8..64 lowercase
  /// hex characters. The client sends a SHA-1; the limit also blocks `..`.
  static bool valid_batch(const std::string& batch);

 private:
  uint64_t now_ms() const;
  void sweep_locked(uint64_t now);
  void sweep_orphan_files_locked(uint64_t now);
  bool ensure_directory_locked();
  std::string unique_final_path_locked(const std::string& name,
                                       const std::string& directory) const;

  std::string directory_;
  files_applier applier_;
  clock_fn clock_;
  mutable std::mutex mutex_;
  std::map<std::string, staged_clipboard_entry> entries_;
  uint64_t last_orphan_sweep_ms_ = 0;
};

}  // namespace molink
