#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "clipboard/virtual_mount.h"

namespace molink {

/// Largest range one read asks the source for. A paste of a large file is
/// split into `clipboard-pull` ranges of this size, regardless of how many
/// bytes the consumer requested in one `IStream::Read`.
constexpr uint32_t kVirtualStreamBlockBytes = 256u * 1024u;

enum class virtual_stream_status {
  ok,
  /// The source refused the range, or it answered something invalid.
  read_error,
  /// The offer was closed (session gone, clipboard replaced). A reader that
  /// is still inside a blocking read returns as soon as its source wakes it.
  stopped,
};

struct virtual_stream_read_result {
  virtual_stream_status status = virtual_stream_status::ok;
  uint32_t bytes = 0;
};

enum class virtual_stream_seek_origin { begin, current, end };

/// IStream position arithmetic. Seeking past the end is allowed; a negative
/// result or an overflow is refused.
bool virtual_stream_seek(uint64_t size, uint64_t position, int64_t move,
                         virtual_stream_seek_origin origin,
                         uint64_t& new_position);

/// The files of one offer plus the reader that answers their ranges.
///
/// Platform-neutral on purpose: the Windows data object wraps it in an
/// `IStream`, and the tests exercise the same byte accounting without COM.
/// `stop()` detaches the reader so a stream Explorer still holds fails
/// instead of calling into a publisher that is shutting down.
class virtual_stream_source {
 public:
  virtual_stream_source(std::vector<virtual_mount_file> files,
                        virtual_file_reader* reader);
  virtual_stream_source(const virtual_stream_source&) = delete;
  virtual_stream_source& operator=(const virtual_stream_source&) = delete;

  /// Closed offers never touch the reader again.
  void stop();
  bool stopped() const;

  std::size_t size() const;
  const virtual_mount_file& file(std::size_t index) const;
  /// The immutable descriptor list, used to build FILEGROUPDESCRIPTORW.
  const std::vector<virtual_mount_file>& files() const { return files_; }

  /// Reads at most `kVirtualStreamBlockBytes` of file `index` at `offset`.
  /// The reader may block; no lock is held across that call. A failure is
  /// reported as `read_error`, never as a short successful read: the viewer
  /// already declared the file size, so a failed range must make the copy
  /// dialog report an error.
  virtual_stream_read_result read(std::size_t index, uint64_t offset,
                                  void* buffer, uint32_t capacity) const;

 private:
  mutable std::mutex mutex_;
  std::vector<virtual_mount_file> files_;
  virtual_file_reader* reader_ = nullptr;
  bool stopped_ = false;
};

/// One stream's seek position. Kept separate from the source because Explorer
/// may hold several streams (or clones) over the same offer.
class virtual_stream_cursor {
 public:
  virtual_stream_cursor(std::shared_ptr<virtual_stream_source> source,
                        std::size_t index);

  uint64_t size() const;
  uint64_t position() const;

  bool seek(int64_t move, virtual_stream_seek_origin origin,
            uint64_t& new_position);

  /// Reads at the current position and advances it by the bytes returned.
  virtual_stream_read_result read(void* buffer, uint32_t capacity);

 private:
  std::shared_ptr<virtual_stream_source> source_;
  std::size_t index_ = 0;
  mutable std::mutex mutex_;
  uint64_t position_ = 0;
};

}  // namespace molink
