#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace molink {

/// Per-session file transfer.
///
/// Uploads (remote client -> host) land in ~/moLink unless the viewer picks a
/// destination. Downloads and directory listings cover every folder the host
/// user can read: there is no home sandbox, so Windows drives, UNC shares and
/// POSIX `/etc` are all browsable. Data moves in binary `file_data` packets;
/// control messages are JSON `file` actions. Downloads are pull-driven so a
/// fast disk cannot flood the socket.
class file_transfer {
 public:
  using json_sender = std::function<void(std::string)>;
  using chunk_sender =
      std::function<bool(uint32_t id, uint64_t offset, const uint8_t* data,
                         std::size_t len)>;

  using clock = std::chrono::steady_clock;
  static constexpr std::size_t kChunkBytes = 256 * 1024;
  static constexpr uint64_t kMaxFileBytes = 8ull * 1024 * 1024 * 1024;
  /// Acknowledge at least this often, in chunks, and at least this often in
  /// time. Acks are small control messages; batching them keeps a fast upload
  /// from turning the control channel into a per-chunk echo.
  static constexpr uint64_t kAckEveryChunks = 4;
  static constexpr auto kAckInterval = std::chrono::milliseconds(200);
  /// A chunk that arrives ahead of the contiguous receive point is answered
  /// with `file-nack` so the viewer can resume there. A viewer that keeps
  /// sending beyond this window, or makes no progress for this long, is
  /// treated as unrecoverable and the upload is failed.
  static constexpr uint64_t kMaxAheadBytes = 16ull * kChunkBytes;
  static constexpr auto kOrderTimeout = std::chrono::seconds(30);

  enum class upload_event { accepted, progress, done, failed };
  /// Upload lifecycle for the delayed clipboard path. `tag` is the opaque
  /// number the host put on the fetch request; ordinary uploads carry -1 and
  /// are ignored by the clipboard listener. `path` is set for `done`.
  using upload_watcher = std::function<void(int tag, upload_event event,
                                            uint32_t id, uint64_t received,
                                            uint64_t size,
                                            const std::string& path)>;

  file_transfer(json_sender send_json, chunk_sender send_chunk);
  ~file_transfer();

  void watch_uploads(upload_watcher watcher);

  /// `name` is a bare file name from the peer; `size` is the declared length.
  /// `destination` is the viewer's chosen folder; empty means ~/moLink. A
  /// relative parent inside `name` (a folder drag) is created underneath it.
  void offer(const std::string& name, uint64_t size,
             const std::string& destination, int tag = -1);
  /// Creates an empty directory (and its parents) under the viewer's chosen
  /// destination. `path` must be relative and is validated like an upload
  /// name; a peer that sends only a bare file name never needs this.
  void mkdir(const std::string& path, const std::string& destination);
  void chunk(uint32_t id, uint64_t offset, const uint8_t* data,
             std::size_t len);
  void complete(uint32_t id, clock::time_point now = clock::now());
  void cancel(uint32_t id);

  /// Lists a folder anywhere on the host. An empty `path` (or `roots=true`)
  /// answers with the volume/root list; every other answer also carries the
  /// current `roots`, so the panel can offer a drive selector without a
  /// second round trip.
  void browse(const std::string& path, bool roots = false);
  void request(const std::string& path,
               clock::time_point now = clock::now());
  static constexpr int kRetryIntervalMs = 100;
  static constexpr auto kRetryTimeout = std::chrono::seconds(20);
  /// An abandoned download or upload holds one of the eight slots and a file
  /// descriptor. Reclaim it when no pull (download) or chunk (upload) has
  /// moved for this long; the viewer that left never sends `cancel`.
  static constexpr auto kIdleTimeout = std::chrono::seconds(120);
  void pull(uint32_t id, clock::time_point now = clock::now());
  void retry_pending(clock::time_point now = clock::now());
  bool has_pending() const;
  /// True while any download or upload is open, including one that is merely
  /// waiting for the viewer's next pull. Keeps the retry timer alive so the
  /// idle reclaim above cannot be starved.
  bool has_transfers() const;
  void fail_pending();

  /// Drop partial uploads when the session closes.
  void close_all();

  /// Resolve peer-supplied paths to existing files anywhere on the host.
  /// Used by the "copy to the remote clipboard" action.
  std::vector<std::string> resolve_files(
      const std::vector<std::string>& paths) const;

 private:
  struct upload {
    uint32_t id = 0;
    int tag = -1;
    std::string name;
    std::string temp_path;
    std::string final_dir;
    uint64_t size = 0;
    uint64_t received = 0;
    /// Contiguous offset already acknowledged to the viewer, and the ack
    /// pacing state.
    uint64_t acked = 0;
    uint64_t chunks_since_ack = 0;
    clock::time_point last_ack = clock::now();
    /// Set when a chunk arrived beyond `received`; cleared by any progress.
    std::optional<clock::time_point> disorder_since;
    std::ofstream out;
    std::optional<clock::time_point> complete_deadline;
    clock::time_point last_activity = clock::now();
  };

  struct download {
    uint32_t id = 0;
    std::string name;
    std::string path;
    uint64_t size = 0;
    uint64_t sent = 0;
    std::ifstream in;
    std::vector<uint8_t> pending_chunk;
    std::optional<clock::time_point> retry_deadline;
    clock::time_point last_activity = clock::now();
  };

  // Returns true when the pull is complete or failed and the item is done.
  bool send_next(download& item, clock::time_point now);

  void fail(const std::string& action, uint32_t id, const std::string& why);
  /// Fail an upload: `cancel` keeps older viewers working, `file-failed`
  /// carries the file name and reason for the batch-failure summary.
  void fail_upload(upload& item, const std::string& why);
  void send_ack(upload& item);
  void send_nack(upload& item);
  void maybe_ack(upload& item, bool force);
  void send_simple(const std::string& action, uint32_t id = 0);
  void drop_upload(uint32_t id, bool remove_file);
  void notify_upload(const upload& item, upload_event event,
                     const std::string& path = std::string());
  /// Creates the upload's temporary file without ever reusing a name that is
  /// already on disk. Updates `item.temp_path`; false on failure.
  bool create_temp_upload_file(upload& item, const std::string& dir);

  json_sender send_json_;
  chunk_sender send_chunk_;
  upload_watcher upload_watcher_;
  std::map<uint32_t, upload> uploads_;
  std::map<uint32_t, download> downloads_;
  uint32_t next_id_ = 1;
  /// Random per-instance tag in the `.part` name. Each session owns its own
  /// file_transfer, and the id counter restarts at 1, so the bare id used to
  /// collide between two viewers uploading into the same folder.
  std::string temp_tag_;
};

}  // namespace molink
