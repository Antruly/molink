#pragma once

#include <cstdint>
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

  static constexpr std::size_t kChunkBytes = 256 * 1024;
  static constexpr uint64_t kMaxFileBytes = 8ull * 1024 * 1024 * 1024;

  file_transfer(json_sender send_json, chunk_sender send_chunk);
  ~file_transfer();

  /// `name` is a bare file name from the peer; `size` is the declared length.
  void offer(const std::string& name, uint64_t size);
  void chunk(uint32_t id, uint64_t offset, const uint8_t* data,
             std::size_t len);
  void complete(uint32_t id);
  void cancel(uint32_t id);

  /// Empty `path` means the home directory. Paths outside home are rejected.
  void browse(const std::string& path);
  void request(const std::string& path);
  void pull(uint32_t id);

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
  };

  struct download {
    uint32_t id = 0;
    std::string name;
    std::string path;
    uint64_t size = 0;
    uint64_t sent = 0;
    std::ifstream in;
  };

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
