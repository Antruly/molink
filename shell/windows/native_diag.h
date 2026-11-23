// Local diagnostics for the native client.
//
// This mirrors `backend/src/diag_log.*` line for line: one file per local day
// (`molink-YYYYMMDD.log`), seven days of retention, one 32 MiB roll to `.1`,
// a bounded in-memory ring, and the same redaction rules. The core's events
// already carry their component as a prefix (`session.connect`,
// `transport.rtt`, `video.mode`), so the on-disk line is identical whether it
// was written by the host or by this client:
//
//   <timestamp> <LEVEL> <component.event> key=value ... :: message
//
// Two levels the core never emits (`TRACE`, `DEBUG`) exist for native-only
// debugging. Everything here is platform-neutral: Win32 is used where needed
// and POSIX on the Linux unit-test build, with no `std::filesystem` (the Win7
// x86 kit's GCC 8 cannot compile it).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace molink::native_diag {

/// One structured field on a diagnostic line.
struct field {
  std::string key;
  std::string value;
};

enum class level { trace, debug, info, warn, error };

/// The parts of the client a line can belong to. Backend events use the same
/// names as their event prefix.
enum class component {
  session,
  transport,
  video,
  clipboard,
  files,
  input,
  auth,
  ui,
};

constexpr uint64_t kDefaultMaxFileBytes = 32ull * 1024 * 1024;
constexpr int kDefaultRetentionDays = 7;
constexpr std::size_t kDefaultRingLines = 2000;
constexpr uint64_t kTailByteLimit = 256ull * 1024;

class logger {
 public:
  explicit logger(std::string directory, bool enabled = true,
                  int retention_days = kDefaultRetentionDays,
                  uint64_t max_file_bytes = kDefaultMaxFileBytes,
                  std::function<int64_t()> now_ms = {},
                  std::size_t ring_lines = kDefaultRingLines);
  ~logger();

  logger(const logger&) = delete;
  logger& operator=(const logger&) = delete;

  /// Switching off keeps the ring and closes the file. It never creates the
  /// directory and never deletes existing files.
  void set_enabled(bool enabled);
  bool enabled() const;

  /// Minimum severity recorded (to the ring and the file). Defaults to info:
  /// trace/debug are opt-in, matching the core's info/warn/error output.
  void set_level(level minimum);
  level minimum_level() const;

  void log(level severity, component part, const std::string& event,
           const std::string& message = std::string(),
           const std::vector<field>& fields = {});

  /// Newest `lines` lines. The second form drops lines below `minimum` before
  /// trimming; both cap the result at 256 KiB.
  std::string tail(int lines) const;
  std::string tail(int lines, level minimum) const;

  struct state_info {
    std::string directory;
    int retention_days = kDefaultRetentionDays;
    uint64_t current_bytes = 0;
    bool enabled = false;
    std::string current_file;
    level minimum = level::info;
  };
  state_info describe() const;

  /// Creates the log directory. Returns true when it exists afterwards.
  bool ensure_directory();

  /// Removes every `molink-YYYYMMDD.log*` file and clears the ring.
  std::size_t clear();

  /// Removes files outside the retention window. Does nothing when disabled
  /// or when the directory does not exist.
  std::size_t cleanup_old_files();

  const std::string& directory() const { return directory_; }

  // ---- pure helpers, exposed for tests --------------------------------
  static bool keep_day(int file_day, int today_day, int retention_days);
  static bool level_at_least(level value, level minimum);
  static const char* level_name(level value);
  static const char* component_name(component value);
  static bool is_secret_key(const std::string& key);
  static std::string sanitize_message(const std::string& message);
  static std::string format_line(int64_t epoch_ms, level severity,
                                 component part, const std::string& event,
                                 const std::string& message,
                                 const std::vector<field>& fields);
  static std::string file_name_for_day(int64_t epoch_ms);
  static int day_number(int64_t epoch_ms);
  /// Severity token of an already formatted line (trace when unreadable).
  static level line_level(const std::string& line);

 private:
  int64_t now_ms() const;
  std::vector<std::string> gather_locked(std::size_t want) const;
  std::string render_locked(std::vector<std::string> lines) const;
  void write_locked(const std::string& line, int64_t now);
  void open_for_day_locked(int64_t now);
  void rotate_locked();
  void push_ring_locked(const std::string& line);
  std::size_t cleanup_old_files_locked(int today);

  std::string directory_;
  bool enabled_;
  level minimum_level_ = level::info;
  int retention_days_;
  uint64_t max_file_bytes_;
  std::function<int64_t()> now_ms_;
  std::size_t ring_capacity_;

  mutable std::mutex mutex_;
  std::deque<std::string> ring_;
  std::string current_path_;
  uint64_t current_bytes_ = 0;
  int open_day_ = -1;
  int last_cleanup_day_ = -1;
  bool file_broken_ = false;
};

/// Process-wide logger. On Windows the directory is
/// `%LOCALAPPDATA%\moLink\logs`, matching the task's convention; the core uses
/// its own (ProgramData) directory, and the two are compared by line, not by
/// path.
logger& diag();
std::string default_directory();

}  // namespace molink::native_diag
