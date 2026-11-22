#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
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

/// Validates the descriptors of one offer. Names become CF_HDROP path
/// components, so separators and control characters are rejected.
lazy_offer_error validate_lazy_offer(
    const std::vector<lazy_clipboard_file>& files);

/// One offered file list plus the fetch pipeline behind it.
///
/// Windows hands the clipboard render call to this class from the clipboard
/// thread. `ensure()` blocks on a condition variable until the bytes arrive
/// (or the deadline expires); the network never runs on the calling thread --
/// `request` posts the fetch to the session's event loop, and the upload
/// progress/completion notifications arrive on that loop.
class lazy_file_source {
 public:
  using request_fn = std::function<void(std::size_t index)>;

  lazy_file_source(std::vector<lazy_clipboard_file> files,
                   request_fn request);
  ~lazy_file_source();

  lazy_file_source(const lazy_file_source&) = delete;
  lazy_file_source& operator=(const lazy_file_source&) = delete;

  const std::vector<lazy_clipboard_file>& files() const { return files_; }
  std::size_t size() const { return files_.size(); }

  /// Blocks until file `index` is on disk. Returns false on timeout, session
  /// loss, or a failed transfer. `path` is only valid when this returns true.
  bool ensure(std::size_t index, std::string& path,
              uint64_t total_timeout_ms = kLazyRenderTimeoutMs,
              uint64_t chunk_timeout_ms = kLazyChunkTimeoutMs);

  /// Waits for every file with one shared total deadline, so an Explorer paste
  /// of eight files cannot turn into eight consecutive 15 s waits.
  bool ensure_all(std::vector<std::string>& paths,
                  uint64_t total_timeout_ms = kLazyRenderTimeoutMs,
                  uint64_t chunk_timeout_ms = kLazyChunkTimeoutMs);

  /// Upload progress, called on the session's event loop.
  void note_progress(std::size_t index, uint64_t received);
  void note_done(std::size_t index, const std::string& path);
  void note_failed(std::size_t index);
  /// Session gone or the offer replaced: wake every waiter with failure.
  void fail_all(const std::string& reason);

  bool closed() const;

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

struct lazy_offer {
  uint32_t session_id = 0;
  std::string dest;
  std::vector<lazy_clipboard_file> files;
  std::shared_ptr<lazy_file_source> source;
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
                         std::shared_ptr<lazy_file_source> source);

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
