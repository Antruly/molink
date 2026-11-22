#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace molink {

/// One file, as it appears inside the virtual clipboard mount.
struct virtual_mount_file {
  /// Session-scoped id from `clipboard-sync.files`; passed back to the reader.
  uint32_t id = 0;
  /// Base name only, already sanitized and unique.
  std::string name;
  uint64_t size = 0;
};

/// Blocking range read, called on a FUSE worker thread. Returns the number of
/// bytes written into `buffer`, or a negative errno (the FUSE layer maps every
/// failure to `-EIO`, which is what a file manager reports when a paste fails
/// after the clipboard changed).
class virtual_file_reader {
 public:
  virtual ~virtual_file_reader() = default;
  virtual int read_file_range(uint32_t file_id, uint64_t offset,
                              uint64_t length, char* buffer,
                              std::size_t capacity) = 0;
};

/// A mounted virtual directory. `stop()` must be idempotent and must join every
/// thread before returning. It returns true when the mount is no longer in the
/// path namespace, so the caller may remove the (now ordinary) directory; a
/// lazy detach still counts as detached.
class virtual_mount {
 public:
  virtual ~virtual_mount() = default;
  virtual const std::string& path() const = 0;
  virtual bool mounted() const = 0;
  virtual bool stop() = 0;
};

using virtual_mount_factory = std::function<std::shared_ptr<virtual_mount>(
    const std::string& mountpoint, const std::vector<virtual_mount_file>& files,
    virtual_file_reader* reader, std::string& error)>;

/// nullptr when this build/platform has no usable FUSE support. The caller
/// then falls back to downloading the selection.
virtual_mount_factory platform_virtual_mount_factory();

/// True when the platform layer was compiled with libfuse. A factory result is
/// still required for a mount to succeed (a missing /dev/fuse or an
/// unprivileged user is reported through `error`).
bool platform_virtual_mount_supported();

/// True when `path` is currently a mountpoint. Implemented by reading
/// /proc/self/mountinfo, never by stat()ing `path`: once the FUSE daemon is
/// gone a stat() on a stale mount can block forever.
bool platform_path_is_mountpoint(const std::string& path);

}  // namespace molink
