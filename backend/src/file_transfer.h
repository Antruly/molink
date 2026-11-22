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
/// Uploads (remote client -> host) land in ~/Downloads/moLink. Downloads and
/// directory listings are confined to the host user's home directory. Data
/// moves in binary `file_data` packets; control messages are JSON `file`
/// actions. Downloads are pull-driven so a fast disk cannot flood the socket.
class file_transfer {
 public:
  using json_sender = std::function<void(std::string)>;
  using chunk_sender =
      std::function<bool(uint32_t id, uint64_t offset, const uint8_t* data,
                         std::size_t len)>;

  using clock = std::chrono::steady_clock;
  static constexpr std::size_t kChunkBytes = 256 * 1024;
  static constexpr uint64_t kMaxFileBytes = 8ull * 1024 * 1024 * 1024;

  file_transfer(json_sender send_json, chunk_sender send_chunk);
  ~file_transfer();

  /// `name` is a bare file name from the peer; `size` is the declared length.
  /// `destination` is the viewer's chosen folder; empty means ~/moLink. A
  /// relative parent inside `name` (a folder drag) is created underneath it.
  void offer(const std::string& name, uint64_t size,
             const std::string& destination);
  /// Creates an empty directory (and its parents) under the viewer's chosen
  /// destination. `path` must be relative and is validated like an upload
  /// name; a peer that sends only a bare file name never needs this.
  void mkdir(const std::string& path, const std::string& destination);
  void chunk(uint32_t id, uint64_t offset, const uint8_t* data,
             std::size_t len);
  void complete(uint32_t id, clock::time_point now = clock::now());
  void cancel(uint32_t id);

  /// Empty `path` means the home directory. Paths outside home are rejected.
  void browse(const std::string& path);
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

  /// Resolve peer-supplied paths to existing files inside the home folder.
  /// Used by the "copy to the remote clipboard" action.
  std::vector<std::string> resolve_files(
      const std::vector<std::string>& paths) const;

 private:
  struct upload {
    uint32_t id = 0;
    std::string name;
    std::string temp_path;
    std::string final_dir;
    uint64_t size = 0;
    uint64_t received = 0;
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
  void send_simple(const std::string& action, uint32_t id = 0);
  void drop_upload(uint32_t id, bool remove_file);

  json_sender send_json_;
  chunk_sender send_chunk_;
  std::map<uint32_t, upload> uploads_;
  std::map<uint32_t, download> downloads_;
  uint32_t next_id_ = 1;
};

}  // namespace molink
