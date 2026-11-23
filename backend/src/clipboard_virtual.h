#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "clipboard/virtual_mount.h"

namespace molink {

/// One file from a `clipboard-sync.files` announcement, as the page forwards
/// it to the local core.
struct virtual_offer_request_file {
  uint32_t id = 0;
  std::string name;
  uint64_t size = 0;
};

enum class virtual_offer_error {
  none = 0,
  /// No FUSE on this build/platform, or the clipboard writer is missing.
  unavailable,
  /// The announcement had no usable entry.
  bad_file,
  /// The directory or the FUSE mount could not be created.
  mount_failed,
  /// The local clipboard refused the mount paths.
  clipboard_unavailable,
};

/// The virtual clipboard exposes at most this many files. The host applies the
/// same cap when it builds the id list.
constexpr std::size_t kVirtualMaxFiles = 8;
/// Sanity bounds only: metadata is a few bytes, so a large file costs nothing
/// until the local user actually pastes it.
constexpr uint64_t kVirtualMaxFileBytes = 1ull * 1024 * 1024 * 1024 * 1024;
constexpr uint64_t kVirtualMaxTotalBytes = 4ull * 1024 * 1024 * 1024 * 1024;
/// One `clipboard-pull` range. The host caps a reply at the same size.
constexpr uint64_t kVirtualPullBytes = 256ull * 1024;
/// One range may wait this long for the page to relay the host reply.
constexpr uint64_t kVirtualPullTimeoutMs = 15000;
/// One FUSE read may split into several ranges; this bounds the whole call.
constexpr uint64_t kVirtualReadTimeoutMs = 60000;

/// Turn the announced list into the files the mount will expose. The first
/// files that fit the count/byte budgets are kept; the rest are counted in
/// `omitted`. Invalid names are skipped, not fatal, unless nothing remains.
virtual_offer_error plan_virtual_offer(
    const std::vector<virtual_offer_request_file>& requested,
    std::vector<virtual_mount_file>& planned, std::size_t& omitted);

/// The local side of the virtual clipboard.
///
/// One generation exists at a time: `offer()` stops the previous mount, creates
/// a new read-only directory and puts its paths on the OS clipboard. A FUSE
/// read becomes a `pull` text message on the page's loopback bridge socket; the
/// page forwards it to the host and posts the 0x18 reply back as a binary
/// message. Reads block on a condition variable, never on the page's event
/// loop. Every failure surfaces as `-EIO`, so a paste that raced a clipboard
/// change fails visibly instead of writing a truncated file.
class virtual_clipboard final
    : public virtual_file_reader,
      public std::enable_shared_from_this<virtual_clipboard> {
 public:
  using apply_fn = std::function<bool(const std::vector<std::string>&)>;
  using clear_fn = std::function<void()>;
  using post_fn = std::function<void(std::function<void()>)>;
  using send_text_fn = std::function<void(const std::string&)>;

  /// `pull_timeout_ms` / `read_timeout_ms` default to the production budgets;
  /// tests shrink them so a stalled bridge is exercised without a 15 s wait.
  virtual_clipboard(std::string cache_root, virtual_mount_factory factory,
                    apply_fn apply, clear_fn clear, post_fn post = {},
                    uint64_t pull_timeout_ms = kVirtualPullTimeoutMs,
                    uint64_t read_timeout_ms = kVirtualReadTimeoutMs);
  ~virtual_clipboard() override;

  virtual_clipboard(const virtual_clipboard&) = delete;
  virtual_clipboard& operator=(const virtual_clipboard&) = delete;

  /// True when a mount can be attempted at all (FUSE compiled in and a
  /// clipboard writer exists). A missing /dev/fuse still fails per offer.
  bool available() const;

  /// Replace the current generation. On success `mount` is the generation id
  /// and `paths` are the clipboard paths (in announcement order).
  virtual_offer_error offer(const std::string& session,
                            const std::vector<virtual_offer_request_file>& files,
                            std::size_t& omitted, std::string& mount,
                            std::vector<std::string>& paths, std::string& error);

  /// Stop the mount, fail every waiting read and empty the clipboard. Safe to
  /// call from any thread and when nothing is mounted.
  void clear(const std::string& reason = std::string());

  /// Clear only when `mount` is still the active generation. A stale tab's
  /// clipboard change must not unmount another tab's newer copy.
  bool clear_if(const std::string& mount);

  /// Attach the page's loopback bridge to the generation `mount`. Returns false
  /// when that generation is no longer current (the page is too late).
  /// `bridge_id` receives a token that identifies this attachment, so a stale
  /// socket closing later cannot unmount a newer bridge for the same mount.
  bool bridge_attach(const std::string& mount, send_text_fn send_text,
                     uint64_t* bridge_id = nullptr);

  /// The bridge closed. Fails in-flight reads and drops the mount so a later
  /// paste cannot hang: the bytes have no transport any more. `bridge_id` is
  /// the token from `bridge_attach` (0 means "whatever is attached", which is
  /// only used by tests).
  void bridge_detach(const std::string& mount, uint64_t bridge_id = 0);

  /// One binary bridge reply: `[seq u32][status u32][offset u64][bytes]`.
  void bridge_binary(const uint8_t* data, std::size_t length);

  /// Exposed for diagnostics and tests.
  bool active() const;
  std::string active_mount() const;

  // virtual_file_reader
  int read_file_range(uint32_t file_id, uint64_t offset, uint64_t length,
                      char* buffer, std::size_t capacity) override;

 private:
  struct pending_read {
    uint32_t file_id = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    std::vector<uint8_t> data;
    bool failed = false;
    bool done = false;
  };

  void clear_locked();
  void sweep_stale_locked();

  /// One `clipboard-pull` block. Returns bytes copied or a negative errno.
  int pull_block(uint32_t file_id, uint64_t offset, uint64_t length,
                 char* buffer, std::size_t capacity,
                 std::chrono::steady_clock::time_point total_deadline);

  std::string cache_root_;
  virtual_mount_factory factory_;
  apply_fn apply_;
  clear_fn clear_cb_;
  post_fn post_;

  /// Serialises offer()/clear() so two announcements cannot race over the
  /// directory and the clipboard.
  std::mutex lifecycle_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  /// Pending FUSE reads that have not returned yet. clear() gives them a short
  /// window to deliver -EIO before unmounting, so the kernel reports EIO rather
  /// than tearing the connection down under them (ENOTCONN).
  std::condition_variable drain_cv_;
  std::size_t readers_waiting_ = 0;
  std::shared_ptr<virtual_mount> mount_;
  std::string mount_id_;
  std::string mountpoint_;
  std::string session_;
  std::map<uint32_t, virtual_mount_file> files_;
  std::map<uint32_t, std::shared_ptr<pending_read>> pending_;
  /// Recently served ranges. The mount is immutable, so a repeated read (the
  /// kernel's readahead and an application's explicit read often overlap) can
  /// be answered locally instead of pulling the same bytes twice.
  struct cached_range {
    uint64_t length = 0;
    std::vector<uint8_t> data;
  };
  std::map<std::pair<uint32_t, uint64_t>, cached_range> cache_;
  std::deque<std::pair<uint32_t, uint64_t>> cache_order_;
  std::size_t cache_bytes_ = 0;
  static constexpr std::size_t kCacheMaxBytes = 4ull * 1024 * 1024;
  /// Directories whose lazy detach had not completed when the generation was
  /// cleared. Retried on the next offer instead of being removed while a stale
  /// mount could still block the caller in the kernel.
  std::vector<std::string> stale_dirs_;
  uint32_t next_seq_ = 1;
  uint64_t next_generation_ = 0;
  send_text_fn send_;
  bool bridge_ = false;
  uint64_t next_bridge_id_ = 1;
  uint64_t bridge_id_ = 0;
  uint64_t pull_timeout_ms_ = kVirtualPullTimeoutMs;
  uint64_t read_timeout_ms_ = kVirtualReadTimeoutMs;
};

}  // namespace molink
