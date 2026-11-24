#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
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
  /// A closed session keeps an unfinished `.part` for this long so the same
  /// viewer can reconnect and offer the same file with an `offset`. Explicit
  /// cancel, a completed upload and an idle reclaim still delete it at once.
  static constexpr auto kResumeGrace = std::chrono::seconds(60);

  /// Optional write sandbox. Off by default: the file channel deliberately
  /// lets a viewer write any folder the host user can create. When enabled,
  /// uploads and `mkdir` must resolve inside one of `roots` (raw text; `~` is
  /// the host home, and an empty list means `~/moLink`).
  struct write_policy {
    bool enabled = false;
    std::vector<std::string> roots;
  };
  using write_policy_provider = std::function<write_policy()>;

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
  /// The id logged on every `file.*` diagnostic. Optional: tests and the
  /// clipboard-only paths simply leave it empty.
  void set_session_id(std::string id);
  /// Called for every upload and `mkdir`. An empty provider, or a disabled
  /// policy, keeps the historical "any folder" behaviour.
  void set_write_policy(write_policy_provider provider);

  /// `name` is a bare file name from the peer; `size` is the declared length.
  /// `destination` is the viewer's chosen folder; empty means ~/moLink. A
  /// relative parent inside `name` (a folder drag) is created underneath it.
  /// `offset`/`digest` are the additive resume hint: the viewer already has
  /// the first `offset` bytes and (optionally) their SHA-256. `resume` is an
  /// opaque, client-stable key that ties two sessions of the same logical
  /// upload to one `.part`; an older client sends neither and starts at 0.
  void offer(const std::string& name, uint64_t size,
             const std::string& destination, int tag = -1,
             uint64_t offset = 0,
             const std::string& digest = std::string(),
             const std::string& resume = std::string());
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
  /// Resume a download at `offset`. Old viewers keep the first overload; a
  /// viewer that reconnects asks for the byte it already saved.
  void pull(uint32_t id, uint64_t offset,
            clock::time_point now = clock::now());
  void retry_pending(clock::time_point now = clock::now());
  bool has_pending() const;
  /// True while any download or upload is open, including one that is merely
  /// waiting for the viewer's next pull. Keeps the retry timer alive so the
  /// idle reclaim above cannot be starved.
  bool has_transfers() const;
  void fail_pending();

  /// Drop partial uploads when the session closes. An unfinished `.part` is
  /// kept for `kResumeGrace` so a reconnecting viewer can resume it; the
  /// supervisor (or the next constructor) sweeps expired ones.
  void close_all();
  /// Delete `.part` files whose resume grace has elapsed. Public static so
  /// tests can drive the clock without a running supervisor.
  static void sweep_orphans(clock::time_point now = clock::now());

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
    /// Identity shared by every attempt at this logical upload. It is part of
    /// the `.part` name, so a new session can find the file the previous one
    /// left behind without guessing.
    std::string ident;
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
  void pull_impl(uint32_t id, std::optional<uint64_t> offset,
                 clock::time_point now);

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
  /// Finds a `.part` for the same logical upload left by a previous session
  /// and opens it for append. False when there is nothing safe to resume.
  bool adopt_temp_upload_file(upload& item, const std::string& dir,
                              uint64_t offset, const std::string& digest);
  write_policy current_write_policy() const;
  /// True when @p target is inside one of the configured roots. `root` is the
  /// matching canonical root in UTF-8.
  bool sandbox_allows(const std::filesystem::path& target,
                      const write_policy& policy, std::string& root) const;
  void deny_path(const std::string& action,
                 const std::filesystem::path& target,
                 const write_policy& policy);

  json_sender send_json_;
  chunk_sender send_chunk_;
  upload_watcher upload_watcher_;
  write_policy_provider write_policy_provider_;
  std::string session_id_;
  std::map<uint32_t, upload> uploads_;
  std::map<uint32_t, download> downloads_;
  uint32_t next_id_ = 1;
  /// Random per-instance tag in the `.part` name. Each session owns its own
  /// file_transfer, and the id counter restarts at 1, so the bare id used to
  /// collide between two viewers uploading into the same folder.
  std::string temp_tag_;
};

}  // namespace molink
