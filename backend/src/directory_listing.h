#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace molink {
namespace fslist {

/// One directory entry as shown by the file panel. `error` is set when the
/// name could be read but its metadata could not; the row is still listed so
/// one unreadable entry never hides the rest of the folder.
struct entry {
  std::string name;
  bool dir = false;
  /// Dot file (POSIX/Explorer convention) or FILE_ATTRIBUTE_HIDDEN.
  bool hidden = false;
  /// FILE_ATTRIBUTE_SYSTEM on Windows; always false elsewhere.
  bool system = false;
  /// Symlink or a Windows reparse point (junction). Marked so the UI can warn,
  /// but still enterable: only recursive walks refuse to follow it.
  bool link = false;
  uint64_t size = 0;
  uint64_t mtime = 0;
  std::string error;
};

struct listing {
  /// Native absolute path (`C:\Windows` or `/var/log`).
  std::string path;
  /// Parent folder, or empty when `path` is a volume root. The remote panel
  /// turns an empty parent into its volume list.
  std::string parent;
  std::vector<entry> entries;
  bool truncated = false;
};

enum class root_kind { fixed, removable, network, cdrom, ram, unknown, home };

struct root {
  std::string name;
  std::string path;
  root_kind kind = root_kind::unknown;
};

const char* root_kind_name(root_kind kind);

/// The volumes this machine exposes: drive letters on Windows (including
/// mapped network drives), `/` plus the home shortcut elsewhere.
std::vector<root> system_roots(const std::string& home);

/// Pure drive-mask mapping shared by `system_roots` and the unit tests.
/// `drive_type` receives 0=A .. 25=Z and returns the Win32 `DRIVE_*` value.
std::vector<root> roots_from_drives(uint32_t mask,
                                    const std::function<int(int)>& drive_type,
                                    const std::string& home);

/// Lexically normalise an absolute path, keeping a drive or UNC prefix.
/// Empty input is not a path; the caller decides whether that means the
/// volume list (remote browse) or the home folder (the local pane).
/// Relative input and `~` resolve against `home`. `..` is collapsed and can
/// never climb above a volume root, so the result is always absolute.
std::optional<std::string> normalize_posix_path(const std::string& raw,
                                                const std::string& home);
std::optional<std::string> normalize_windows_path(const std::string& raw,
                                                  const std::string& home);

/// The platform's rule set.
std::optional<std::string> normalize_browse_path(const std::string& raw,
                                                 const std::string& home);

/// `path.string()` with native separators (Windows may keep the `/` the user
/// typed; the panel and Explorer both prefer `\`).
std::string native_path(const std::filesystem::path& path);

/// List one folder without following the home sandbox. On failure `error`
/// carries a short readable reason ("no such folder", "permission denied",
/// "not a folder", "could not read the folder").
std::optional<listing> list_directory(const std::string& directory,
                                      std::string& error);

struct walk_entry {
  /// Relative to the walk root, '/'-separated.
  std::string path;
  bool dir = false;
  uint64_t size = 0;
  bool hidden = false;
  bool link = false;
};

struct walk_result {
  std::vector<walk_entry> entries;
  bool truncated = false;
};

/// Recursively enumerate a folder for an upload. Symlinked/reparse-point
/// folders are reported but not descended into, and both the entry count and
/// the depth are capped, so a junction loop cannot recurse forever.
walk_result walk_directory(const std::string& directory,
                           std::size_t max_entries = 20000,
                           std::size_t max_depth = 64);

/// `target` if it does not exist, otherwise `name (1).ext`, `name (2).ext`…
/// in the same folder. Used when a received file would overwrite another.
std::string unique_destination(const std::string& target);

}  // namespace fslist
}  // namespace molink
