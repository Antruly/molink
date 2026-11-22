#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace molink {

/// A file the viewer copied locally and offered to the host clipboard without
/// sending its bytes. The host registers only this descriptor; the bytes are
/// fetched the first time a remote application actually pastes.
struct lazy_clipboard_file {
  std::string name;
  uint64_t size = 0;
};

enum class lazy_offer_error {
  none = 0,
  too_many_files,
  too_large,
  bad_file,
  no_session,
};

/// Host-side limits for one lazy clipboard offer.
constexpr std::size_t kMaxLazyFiles = 8;
constexpr uint64_t kMaxLazyBytes = 2ull * 1024 * 1024 * 1024;

/// Delayed rendering budget, matching the clipboard contract: one whole file
/// may take at most 15 s, and a single stalled block fails after 10 s.
constexpr uint64_t kLazyRenderTimeoutMs = 15000;
constexpr uint64_t kLazyChunkTimeoutMs = 10000;

/// Virtual-file streaming. One IStream::Read may span many 64 KiB fetches; a
/// single fetch that does not answer within 10 s fails the read, and the whole
/// Read call has a 60 s budget. Both are deliberately finite: Explorer's copy
/// dialog must report a failure instead of hanging on a dead peer.
constexpr uint64_t kLazyStreamChunkTimeoutMs = 10000;
constexpr uint64_t kLazyStreamTotalTimeoutMs = 60000;
constexpr uint32_t kLazyStreamBlockBytes = 64u * 1024u;

/// Validates the descriptors of one offer. Names become CF_HDROP path
/// components, so separators and control characters are rejected.
lazy_offer_error validate_lazy_offer(
    const std::vector<lazy_clipboard_file>& files);

/// The bytes behind one clipboard offer. Two implementations exist:
///
///  * `lazy_file_source` downloads each whole file into the host's staging
///    directory first (the pre-streaming behaviour), then hands out a path;
///  * `remote_file_source` answers a read for one byte range at a time, so the
///    host can write Explorer's paste straight into the destination folder.
///
/// The clipboard data object asks `streaming()` and chooses a CF_HDROP or a
/// FileContents IStream accordingly.
class lazy_blob_source {
 public:
  virtual ~lazy_blob_source() = default;

  virtual const std::vector<lazy_clipboard_file>& files() const = 0;
  virtual std::size_t size() const = 0;

  /// True when `read_range` is the supported way to obtain bytes and no file
  /// is ever staged on the host's disk.
  virtual bool streaming() const { return false; }

  /// Blocks until file `index` is on disk (eager path only).
  virtual bool ensure(std::size_t, std::string&,
                      uint64_t = kLazyRenderTimeoutMs,
                      uint64_t = kLazyChunkTimeoutMs) {
    return false;
  }
  virtual bool ensure_all(std::vector<std::string>&,
                          uint64_t = kLazyRenderTimeoutMs,
                          uint64_t = kLazyChunkTimeoutMs) {
    return false;
  }

  /// Blocks until `[offset, offset+length)` of file `index` arrives. The
  /// returned chunk may be shorter than `length` at end of file. Only
  /// streaming sources implement this.
  virtual bool read_range(std::size_t, uint64_t, uint32_t,
                          std::vector<uint8_t>&, std::string&,
                          uint64_t = kLazyStreamChunkTimeoutMs,
                          uint64_t = kLazyStreamTotalTimeoutMs) {
    return false;
  }

  virtual void note_progress(std::size_t, uint64_t) {}
  virtual void note_done(std::size_t, const std::string&) {}
  virtual void note_failed(std::size_t) {}
  /// A stream reply from the client: u32 request id, u32 error, u64 offset.
  virtual void note_range(uint32_t, uint32_t, uint64_t, const uint8_t*,
                          std::size_t) {}

  /// Session gone or the offer replaced: wake every waiter with failure.
  virtual void fail_all(const std::string& reason) = 0;
  virtual bool closed() const = 0;
};

/// Position arithmetic for the virtual-file IStream, kept out of the Windows
/// translation unit so it can be tested on every platform.
enum class lazy_seek_origin { begin, current, end };

/// Computes the new absolute position. Returns false for a negative result or
/// an arithmetic overflow; seeking past the end is allowed (as IStream does).
bool lazy_blob_seek(uint64_t size, uint64_t position, int64_t move,
                    lazy_seek_origin origin, uint64_t& new_position);

/// Reads at most `bytes` from `position` through `source`, one kLazyStreamBlock
/// bytes at a time, and advances `position`. Returns false on timeout or a
/// failed range; `read` counts the bytes produced before that. A short range
/// is treated as end of file.
bool read_lazy_blob(lazy_blob_source& source, std::size_t index,
                    uint64_t& position, uint8_t* buffer, uint32_t bytes,
                    uint32_t& read, std::string& error,
                    uint64_t chunk_timeout_ms = kLazyStreamChunkTimeoutMs,
                    uint64_t total_timeout_ms = kLazyStreamTotalTimeoutMs);

/// One offered file list plus the fetch pipeline behind it.
///
/// Windows hands the clipboard render call to this class from the clipboard
/// thread. `ensure()` blocks on a condition variable until the bytes arrive
/// (or the deadline expires); the network never runs on the calling thread --
/// `request` posts the fetch to the session's event loop, and the upload
/// progress/completion notifications arrive on that loop.
class lazy_file_source : public lazy_blob_source {
 public:
  using request_fn = std::function<void(std::size_t index)>;

  lazy_file_source(std::vector<lazy_clipboard_file> files,
                   request_fn request);
  ~lazy_file_source() override;

  lazy_file_source(const lazy_file_source&) = delete;
  lazy_file_source& operator=(const lazy_file_source&) = delete;

  const std::vector<lazy_clipboard_file>& files() const override {
    return files_;
  }
  std::size_t size() const override { return files_.size(); }

  /// Blocks until file `index` is on disk. Returns false on timeout, session
  /// loss, or a failed transfer. `path` is only valid when this returns true.
  bool ensure(std::size_t index, std::string& path,
              uint64_t total_timeout_ms = kLazyRenderTimeoutMs,
              uint64_t chunk_timeout_ms = kLazyChunkTimeoutMs) override;

  /// Waits for every file with one shared total deadline, so an Explorer paste
  /// of eight files cannot turn into eight consecutive 15 s waits.
  bool ensure_all(std::vector<std::string>& paths,
                  uint64_t total_timeout_ms = kLazyRenderTimeoutMs,
                  uint64_t chunk_timeout_ms = kLazyChunkTimeoutMs) override;

  /// Upload progress, called on the session's event loop.
  void note_progress(std::size_t index, uint64_t received) override;
  void note_done(std::size_t index, const std::string& path) override;
  void note_failed(std::size_t index) override;
  /// Session gone or the offer replaced: wake every waiter with failure.
  void fail_all(const std::string& reason) override;

  bool closed() const override;

 private:
  struct file_state {
    bool requested = false;
    bool complete = false;
    bool failed = false;
    uint64_t received = 0;
    std::string path;
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point last_progress;
  };

  bool wait_files(const std::vector<std::size_t>& indices,
                  std::vector<std::string>& paths, uint64_t total_timeout_ms,
                  uint64_t chunk_timeout_ms);

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<lazy_clipboard_file> files_;
  std::vector<file_state> states_;
  request_fn request_;
  bool closed_ = false;
};

/// Streaming source: every range read becomes one `fetch` message to the
/// viewer, and the reply arrives later on the session's event loop. The
/// waiting thread is the COM caller (Explorer's paste), never the event loop.
///
/// A single source serves all files of one offer. Request ids are allocated
/// here, so replies are matched to waiters without relying on what the client
/// echoes back.
class remote_file_source final : public lazy_blob_source {
 public:
  /// Returns false when the request could not even be posted. Posting to the
  /// event loop alone is not enough to know: a session that closes before the
  /// message leaves is handled by fail_all() waking the waiter.
  using range_request_fn = std::function<bool(
      std::size_t index, uint32_t id, uint64_t offset, uint32_t length)>;

  remote_file_source(std::vector<lazy_clipboard_file> files,
                     range_request_fn request);
  ~remote_file_source() override;

  remote_file_source(const remote_file_source&) = delete;
  remote_file_source& operator=(const remote_file_source&) = delete;

  const std::vector<lazy_clipboard_file>& files() const override {
    return files_;
  }
  std::size_t size() const override { return files_.size(); }
  bool streaming() const override { return true; }

  bool read_range(std::size_t index, uint64_t offset, uint32_t length,
                  std::vector<uint8_t>& out, std::string& error,
                  uint64_t chunk_timeout_ms = kLazyStreamChunkTimeoutMs,
                  uint64_t total_timeout_ms = kLazyStreamTotalTimeoutMs)
      override;

  /// Called on the session's event loop for packet `clipboard_range`.
  void note_range(uint32_t id, uint32_t error, uint64_t offset,
                  const uint8_t* data, std::size_t length) override;

  /// True while `id` is still waiting. A `fetch` posted to the event loop may
  /// arrive after the read timed out or the offer was replaced; the session
  /// drops it instead of asking the client for bytes nobody will consume.
  bool has_pending(uint32_t id) const;

  void fail_all(const std::string& reason) override;
  bool closed() const override;

 private:
  struct pending {
    uint64_t offset = 0;
    uint32_t error = 0;
    uint64_t reply_offset = 0;
    std::vector<uint8_t> data;
    bool done = false;
  };

  std::vector<lazy_clipboard_file> files_;
  range_request_fn request_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::map<uint32_t, std::shared_ptr<pending>> pending_;
  bool closed_ = false;
};

struct lazy_offer {
  uint32_t session_id = 0;
  std::string dest;
  std::vector<lazy_clipboard_file> files;
  std::shared_ptr<lazy_blob_source> source;
  uint64_t created_ms = 0;
};

/// The one offer the host's clipboard currently owns. The clipboard is global,
/// so the last viewer's offer wins; replacing it fails the previous source.
class lazy_clipboard {
 public:
  using clock_fn = std::function<uint64_t()>;

  explicit lazy_clipboard(clock_fn clock = {});

  lazy_offer_error offer(uint32_t session_id, std::string dest,
                         std::vector<lazy_clipboard_file> files,
                         std::shared_ptr<lazy_blob_source> source);

  /// Clears the offer only when it belongs to `session_id`. Returns true when
  /// something was cleared, so the caller knows to release the clipboard.
  bool clear_session(uint32_t session_id);
  void clear_all(const std::string& reason);

  std::shared_ptr<const lazy_offer> snapshot() const;
  bool active() const;
  bool active_for(uint32_t session_id) const;
  uint32_t session_id() const;

 private:
  uint64_t now_ms() const;

  mutable std::mutex mutex_;
  std::shared_ptr<lazy_offer> offer_;
  clock_fn clock_;
};

}  // namespace molink
