#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace molink {

/// One structured field on a diagnostic line.
struct diag_field {
  std::string key;
  std::string value;
};

enum class diag_level { info, warn, error };

/// A daily-rolling, local-only diagnostic log.
///
/// * One file per local calendar day (`molink-YYYYMMDD.log`), kept for
///   `retention_days`; older files are removed on the first write of the day
///   and by an explicit cleanup at startup.
/// * A single file is capped at `max_file_bytes`; when it would overflow it
///   is renamed to `...log.1` and a fresh file starts (the previous `.1` is
///   replaced).
/// * Every line is also kept in a small in-memory ring buffer, so `tail`
///   still works while recording is switched off.
/// * Nothing is ever uploaded. The line builder redacts credentials and
///   clipboard content before anything reaches the file or the ring.
///
/// `now_ms` is injectable so retention and rotation can be tested with a
/// fake clock. All methods are thread-safe.
class diag_log {
 public:
  explicit diag_log(std::string directory, bool enabled = true,
                    int retention_days = 7,
                    uint64_t max_file_bytes = 32ull * 1024 * 1024,
                    std::function<int64_t()> now_ms = {},
                    std::size_t ring_lines = 2000);
  ~diag_log();

  diag_log(const diag_log&) = delete;
  diag_log& operator=(const diag_log&) = delete;

  /// Switching off keeps the ring buffer and closes the file. It never
  /// creates the directory and never deletes existing files.
  void set_enabled(bool enabled);
  bool enabled() const;

  void log(diag_level level, const std::string& event,
           const std::string& message = std::string(),
           const std::vector<diag_field>& fields = {});

  /// The newest `lines` lines, newest file first, capped at 256 KiB. When
  /// recording was never enabled (or no file exists) this falls back to the
  /// in-memory ring. An over-long result starts with a truncation marker.
  std::string tail(int lines) const;

  /// Directory, retention, size of the file currently being written and the
  /// recording switch. The directory is reported even while disabled.
  struct info {
    std::string directory;
    int retention_days = 7;
    uint64_t current_bytes = 0;
    bool enabled = false;
    std::string current_file;
  };
  info describe() const;

  /// Removes `molink-YYYYMMDD.log*` files whose day is older than the
  /// retention window. Does nothing when the directory does not exist.
  std::size_t cleanup_old_files();

  const std::string& directory() const { return directory_; }

  // ---- pure helpers, exposed for tests --------------------------------
  static bool is_secret_key(const std::string& key);
  static std::string sanitize_message(const std::string& message);
  static std::string format_line(int64_t epoch_ms, diag_level level,
                                 const std::string& event,
                                 const std::string& message,
                                 const std::vector<diag_field>& fields);
  static std::string file_name_for_day(int64_t epoch_ms);
  static int day_number(int64_t epoch_ms);

 private:
  int64_t now_ms() const;
  void write_locked(const std::string& line, int64_t now);
  void open_for_day_locked(int64_t now);
  void rotate_locked();
  void push_ring_locked(const std::string& line);
  std::size_t cleanup_old_files_locked(int today);

  std::string directory_;
  bool enabled_;
  int retention_days_;
  uint64_t max_file_bytes_;
  std::function<int64_t()> now_ms_;
  std::size_t ring_capacity_;

  mutable std::mutex mutex_;
  std::deque<std::string> ring_;
  /// Open stream state. Empty path means no file is open.
  std::string current_path_;
  uint64_t current_bytes_ = 0;
  int open_day_ = -1;
  int last_cleanup_day_ = -1;
  /// Set after a filesystem error so a broken directory does not retry on
  /// every line (the ring buffer keeps working).
  bool file_broken_ = false;
  mutable std::string last_error_;
};

/// The process-wide logger used by the core. Its directory follows the
/// platform convention (Linux XDG data dir, Windows ProgramData, macOS
/// ~/Library/Logs) unless MOLINK_DIAG_LOG_DIR overrides it.
diag_log& diag();
std::string default_diag_directory();

}  // namespace molink
