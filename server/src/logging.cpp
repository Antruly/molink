#include "logging.h"

#include <cstdarg>
#include <ctime>

namespace molink {
namespace {

constexpr const char* kLevelNames[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};

std::string timestamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

}  // namespace

bool parse_log_level(const std::string& text, log_level& out) {
  if (text == "debug") {
    out = log_level::debug;
    return true;
  }
  if (text == "info") {
    out = log_level::info;
    return true;
  }
  if (text == "warn" || text == "warning") {
    out = log_level::warn;
    return true;
  }
  if (text == "error") {
    out = log_level::error;
    return true;
  }
  return false;
}

const char* log_level_name(log_level level) {
  const int index = static_cast<int>(level);
  if (index < 0 || index >= 4) return "INFO ";
  return kLevelNames[index];
}

logger& logger::instance() {
  static logger instance;
  return instance;
}

bool logger::open(const std::string& file, log_level level, int max_size_mb,
                  int max_files, std::string& err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stream_ != nullptr && stream_ != stderr) std::fclose(stream_);
  stream_ = nullptr;
  file_ = file;
  level_ = level;
  written_ = 0;
  if (max_size_mb > 0) {
    max_bytes_ = static_cast<std::size_t>(max_size_mb) * 1024u * 1024u;
  }
  if (max_files > 0) max_files_ = max_files;
  if (file.empty()) {
    stream_ = stderr;
    return true;
  }
  stream_ = std::fopen(file.c_str(), "a");
  if (stream_ == nullptr) {
    err = "cannot open log file: " + file;
    return false;
  }
  if (std::fseek(stream_, 0, SEEK_END) == 0) {
    const long size = std::ftell(stream_);
    if (size > 0) written_ = static_cast<std::size_t>(size);
  }
  return true;
}

void logger::set_level(log_level level) {
  std::lock_guard<std::mutex> lock(mutex_);
  level_ = level;
}

log_level logger::level() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return level_;
}

bool logger::file_enabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !file_.empty();
}

void logger::write(log_level level, const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (static_cast<int>(level) < static_cast<int>(level_)) return;
  if (stream_ == nullptr) stream_ = stderr;
  if (!file_.empty() && written_ > 0 && written_ >= max_bytes_) {
    rotate_locked();
  }
  const std::string line =
      timestamp() + " [" + log_level_name(level) + "] " + message + "\n";
  std::fwrite(line.data(), 1, line.size(), stream_);
  std::fflush(stream_);
  written_ += line.size();
}

void logger::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stream_ != nullptr) std::fflush(stream_);
}

void logger::rotate_locked() {
  if (stream_ != nullptr && stream_ != stderr) std::fclose(stream_);
  stream_ = nullptr;
  // Shift .N-1 -> .N, keeping at most max_files_ old files.
  for (int index = max_files_ - 1; index >= 1; --index) {
    const std::string from = file_ + "." + std::to_string(index);
    const std::string to = file_ + "." + std::to_string(index + 1);
    std::rename(from.c_str(), to.c_str());
  }
  std::rename(file_.c_str(), (file_ + ".1").c_str());
  stream_ = std::fopen(file_.c_str(), "a");
  if (stream_ == nullptr) stream_ = stderr;
  written_ = 0;
}

void log_message(log_level level, const char* format, ...) {
  char stack[1024];
  va_list args;
  va_start(args, format);
  const int needed = std::vsnprintf(stack, sizeof(stack), format, args);
  va_end(args);
  if (needed < 0) return;
  if (static_cast<std::size_t>(needed) < sizeof(stack)) {
    logger::instance().write(level, std::string(stack, needed));
    return;
  }
  std::string heap(static_cast<std::size_t>(needed) + 1, '\0');
  va_start(args, format);
  std::vsnprintf(heap.data(), heap.size(), format, args);
  va_end(args);
  heap.resize(static_cast<std::size_t>(needed));
  logger::instance().write(level, heap);
}

void log_debug(const char* format, ...) {
  char stack[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(stack, sizeof(stack), format, args);
  va_end(args);
  logger::instance().write(log_level::debug, stack);
}

void log_info(const char* format, ...) {
  char stack[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(stack, sizeof(stack), format, args);
  va_end(args);
  logger::instance().write(log_level::info, stack);
}

void log_warn(const char* format, ...) {
  char stack[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(stack, sizeof(stack), format, args);
  va_end(args);
  logger::instance().write(log_level::warn, stack);
}

void log_error(const char* format, ...) {
  char stack[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(stack, sizeof(stack), format, args);
  va_end(args);
  logger::instance().write(log_level::error, stack);
}

}  // namespace molink
