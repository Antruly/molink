#pragma once

#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>

namespace molink {

enum class log_level { debug = 0, info, warn, error };

bool parse_log_level(const std::string& text, log_level& out);
const char* log_level_name(log_level level);

/// Small process-wide logger. Console mode writes to stderr; service mode can
/// point it at a file and the logger rotates that file by size.
class logger {
 public:
  static logger& instance();

  /// An empty `file` keeps stderr. Returns false only when the file cannot be
  /// opened.
  bool open(const std::string& file, log_level level, int max_size_mb,
            int max_files, std::string& err);
  void set_level(log_level level);
  log_level level() const;
  void write(log_level level, const std::string& message);
  void flush();
  bool file_enabled() const;

 private:
  logger() = default;
  void rotate_locked();

  mutable std::mutex mutex_;
  std::string file_;
  std::FILE* stream_ = nullptr;
  log_level level_ = log_level::info;
  std::size_t max_bytes_ = 20u * 1024u * 1024u;
  int max_files_ = 5;
  std::size_t written_ = 0;
};

void log_message(log_level level, const char* format, ...);
void log_debug(const char* format, ...);
void log_info(const char* format, ...);
void log_warn(const char* format, ...);
void log_error(const char* format, ...);

}  // namespace molink
