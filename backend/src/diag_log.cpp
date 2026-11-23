#include "diag_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <system_error>

namespace molink {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kTailByteLimit = 256ull * 1024;
constexpr std::size_t kFieldLimit = 300;
constexpr std::size_t kPathLimit = 120;
constexpr std::size_t kMessageLimit = 1000;
constexpr std::size_t kLineLimit = 4096;

const char* kFilePrefix = "molink-";
const char* kFileSuffix = ".log";

std::string env_value(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

int64_t system_now_ms() {
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

/// Floor division for negative epochs (dates before 1970 are only relevant
/// for tests, but the arithmetic should still be correct).
int64_t floor_div(int64_t value, int64_t by) {
  const int64_t q = value / by;
  const int64_t r = value % by;
  return (r != 0 && ((r < 0) != (by < 0))) ? q - 1 : q;
}

struct local_time {
  int year = 1970;
  int month = 1;
  int day = 1;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int millis = 0;
  int offset_minutes = 0;
};

/// Portable timegm: interpret the broken-down fields as UTC.
std::time_t utc_from_tm(std::tm* tm_value) {
#if defined(_WIN32)
  return _mkgmtime(tm_value);
#else
  return timegm(tm_value);
#endif
}

/// `localtime`/`gmtime` reuse one static buffer. The logger is called from
/// the app loop, the capture thread and the retention thread, so convert under
/// one lock rather than depending on the platform's `_s`/`_r` flavour (MSVC
/// and MinGW order the arguments of `localtime_s` differently).
std::mutex g_time_mutex;

std::tm local_parts(const std::time_t& t) {
  std::lock_guard<std::mutex> lock(g_time_mutex);
  std::tm out{};
  const std::tm* value = std::localtime(&t);
  if (value != nullptr) out = *value;
  return out;
}


local_time to_local(int64_t epoch_ms) {
  const int64_t seconds = floor_div(epoch_ms, 1000);
  local_time out;
  out.millis = static_cast<int>(epoch_ms - seconds * 1000);
  const std::time_t t = static_cast<std::time_t>(seconds);
  const std::tm local_tm = local_parts(t);
  out.year = local_tm.tm_year + 1900;
  out.month = local_tm.tm_mon + 1;
  out.day = local_tm.tm_mday;
  out.hour = local_tm.tm_hour;
  out.minute = local_tm.tm_min;
  out.second = local_tm.tm_sec;
  // Offset of the local wall clock from UTC at this instant, DST included.
  std::tm copy = local_tm;
  const int64_t local_as_utc = static_cast<int64_t>(utc_from_tm(&copy));
  out.offset_minutes =
      static_cast<int>((local_as_utc - seconds) / 60);
  return out;
}

/// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
int64_t days_from_civil(int year, int month, int day) {
  const int64_t y = static_cast<int64_t>(year) - (month <= 2 ? 1 : 0);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

std::string truncate_utf8(const std::string& value, std::size_t limit) {
  if (value.size() <= limit) return value;
  std::size_t cut = limit;
  // Do not cut in the middle of a UTF-8 code point.
  while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xc0) == 0x80) {
    --cut;
  }
  return value.substr(0, cut) + "...";
}

/// Key names whose values are never written, only the fact that one existed.
bool secret_key_name(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  static const char* const exact[] = {
      "token",      "session",      "code",       "passcode",
      "password",   "passwd",       "secret",     "authorization",
      "cookie",     "text",         "clipboard",  "clip_text",
      "clipboard_text", "content",  "file_content", "payload",
      "verify_code", "verification_code", "temp_code", "long_term_code",
      "session_token", "resume_token",
  };
  for (const char* name : exact) {
    if (key == name) return true;
  }
  return key.find("token") != std::string::npos ||
         key.find("password") != std::string::npos ||
         key.find("passwd") != std::string::npos ||
         key.find("secret") != std::string::npos;
}

std::string quote_value(const std::string& value) {
  if (value.empty()) return "\"\"";
  bool quote = false;
  for (char c : value) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' ||
        c == '=' || c == '\\') {
      quote = true;
      break;
    }
  }
  if (!quote) return value;
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (char c : value) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

std::string normalize_key(std::string key) {
  if (key.empty()) return "field";
  for (char& c : key) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) || c == '_' || c == '-' || c == '.')) c = '_';
  }
  return key;
}

std::string level_name(diag_level level) {
  switch (level) {
    case diag_level::warn:
      return "WARN";
    case diag_level::error:
      return "ERROR";
    case diag_level::info:
    default:
      return "INFO";
  }
}

struct log_file_entry {
  fs::path path;
  int day = 0;
  int part = 0;
};

bool parse_log_file_name(const std::string& name, int& day, int& part) {
  const std::string prefix(kFilePrefix);
  if (name.compare(0, prefix.size(), prefix) != 0) return false;
  std::size_t pos = prefix.size();
  if (name.size() < pos + 8 + std::strlen(kFileSuffix)) return false;
  int value = 0;
  for (int i = 0; i < 8; ++i) {
    const char c = name[pos + i];
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  const int year = value / 10000;
  const int month = (value / 100) % 100;
  const int day_of_month = value % 100;
  if (month < 1 || month > 12 || day_of_month < 1 || day_of_month > 31) {
    return false;
  }
  // Store a linear day number so retention compares like with like.
  const int linear_day =
      static_cast<int>(days_from_civil(year, month, day_of_month));
  pos += 8;
  const std::string suffix(kFileSuffix);
  if (name.compare(pos, suffix.size(), suffix) != 0) return false;
  pos += suffix.size();
  part = 0;
  if (pos == name.size()) {
    day = linear_day;
    return true;
  }
  if (name[pos] != '.') return false;
  ++pos;
  if (pos == name.size()) return false;
  int part_value = 0;
  for (; pos < name.size(); ++pos) {
    const char c = name[pos];
    if (c < '0' || c > '9') return false;
    part_value = part_value * 10 + (c - '0');
  }
  day = linear_day;
  part = part_value;
  return true;
}

std::vector<log_file_entry> list_log_files(const std::string& directory) {
  std::vector<log_file_entry> files;
  std::error_code ec;
  if (directory.empty() || !fs::is_directory(directory, ec)) return files;
  for (fs::directory_iterator it(directory, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string name = it->path().filename().string();
    int day = 0;
    int part = 0;
    if (!parse_log_file_name(name, day, part)) continue;
    log_file_entry entry;
    entry.path = it->path();
    entry.day = day;
    entry.part = part;
    files.push_back(std::move(entry));
  }
  return files;
}

/// Last `want` lines of one file, read backwards so a 32 MiB file is not
/// loaded whole for a 20-line tail.
std::vector<std::string> read_last_lines(const fs::path& path,
                                         std::size_t want) {
  std::vector<std::string> lines;
  if (want == 0) return lines;
  std::ifstream in(path, std::ios::binary);
  if (!in) return lines;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) return lines;
  constexpr std::size_t kChunk = 64 * 1024;
  std::string data;
  std::streamoff pos = size;
  std::size_t newlines = 0;
  while (pos > 0 && newlines <= want) {
    const std::size_t take = static_cast<std::size_t>(
        std::min<std::streamoff>(static_cast<std::streamoff>(kChunk), pos));
    pos -= static_cast<std::streamoff>(take);
    std::string chunk(take, '\0');
    in.seekg(pos);
    in.read(chunk.data(), static_cast<std::streamsize>(take));
    if (in.gcount() != static_cast<std::streamsize>(take)) break;
    data.insert(0, chunk);
    newlines = static_cast<std::size_t>(
        std::count(data.begin(), data.end(), '\n'));
  }
  std::size_t start = 0;
  while (start < data.size()) {
    const std::size_t newline = data.find('\n', start);
    if (newline == std::string::npos) {
      lines.push_back(data.substr(start));
      break;
    }
    lines.push_back(data.substr(start, newline - start));
    start = newline + 1;
  }
  // A trailing newline produces no empty element above, but guard anyway.
  while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
  if (lines.size() > want) {
    lines.erase(lines.begin(),
                lines.end() - static_cast<std::ptrdiff_t>(want));
  }
  return lines;
}

}  // namespace

diag_log::diag_log(std::string directory, bool enabled, int retention_days,
                   uint64_t max_file_bytes,
                   std::function<int64_t()> now_ms, std::size_t ring_lines)
    : directory_(std::move(directory)),
      enabled_(enabled),
      retention_days_(std::max(1, retention_days)),
      max_file_bytes_(max_file_bytes > 0 ? max_file_bytes
                                         : 32ull * 1024 * 1024),
      now_ms_(std::move(now_ms)),
      ring_capacity_(std::max<std::size_t>(1, ring_lines)) {}

diag_log::~diag_log() {
  std::lock_guard<std::mutex> lock(mutex_);
  // The ofstream closes itself; keeping the destructor explicit makes the
  // "close on disable" contract visible.
  current_path_.clear();
}

int64_t diag_log::now_ms() const {
  return now_ms_ ? now_ms_() : system_now_ms();
}

void diag_log::set_enabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  enabled_ = enabled;
  if (!enabled) {
    // Already-written files stay; only the write handle closes.
    current_path_.clear();
    current_bytes_ = 0;
    open_day_ = -1;
    file_broken_ = false;
  }
}

bool diag_log::enabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return enabled_;
}

bool diag_log::is_secret_key(const std::string& key) {
  return secret_key_name(key);
}

std::string diag_log::sanitize_message(const std::string& message) {
  std::string out = message;
  // "unknown key code: MetaLeft" must keep the key name: that line is the
  // whole reason the input-rejection diagnostic exists. Park the phrase, run
  // the credential rules, then put it back.
  const std::string guard = "\x01KEYCODE\x01";
  static const std::regex key_phrase(R"(key\s+code)",
                                     std::regex_constants::icase);
  out = std::regex_replace(out, key_phrase, guard);
  static const std::regex labelled(
      R"((token|session|code|password|passwd|secret)\s*[=:]\s*[^\s,;]+)",
      std::regex_constants::icase);
  out = std::regex_replace(out, labelled, "$1=***");
  // A session token is 32+ hex characters. Hiding any such run is cheap and
  // errs on the safe side.
  static const std::regex hex_run(R"(\b[0-9a-fA-F]{32,}\b)");
  out = std::regex_replace(out, hex_run, "***");
  // Verification codes look like ABC-DEF-GHI. Device IDs share the shape, so
  // free-text IDs are hidden too; they are still available in dedicated
  // fields such as device_name.
  static const std::regex code_shape(R"(\b[A-Z0-9]{3}-[A-Z0-9]{3}-[A-Z0-9]{3}\b)");
  out = std::regex_replace(out, code_shape, "***");
  // Restore every guard without touching the other text.
  std::size_t at = 0;
  while ((at = out.find(guard, at)) != std::string::npos) {
    out.replace(at, guard.size(), "key code");
    at += 8;
  }
  return out;
}

std::string diag_log::file_name_for_day(int64_t epoch_ms) {
  const local_time t = to_local(epoch_ms);
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "%s%04d%02d%02d%s", kFilePrefix,
                t.year, t.month, t.day, kFileSuffix);
  return std::string(buffer);
}

int diag_log::day_number(int64_t epoch_ms) {
  const local_time t = to_local(epoch_ms);
  return static_cast<int>(days_from_civil(t.year, t.month, t.day));
}

std::string diag_log::format_line(int64_t epoch_ms, diag_level level,
                                  const std::string& event,
                                  const std::string& message,
                                  const std::vector<diag_field>& fields) {
  const local_time t = to_local(epoch_ms);
  const int offset = t.offset_minutes;
  const char sign = offset < 0 ? '-' : '+';
  const int abs_offset = offset < 0 ? -offset : offset;
  char stamp[64] = {};
  std::snprintf(stamp, sizeof(stamp),
                "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d", t.year,
                t.month, t.day, t.hour, t.minute, t.second, t.millis, sign,
                abs_offset / 60, abs_offset % 60);
  std::string line(stamp);
  line.push_back(' ');
  line += level_name(level);
  line.push_back(' ');
  line += normalize_key(truncate_utf8(event, 60));
  for (const diag_field& field : fields) {
    const std::string key = normalize_key(field.key);
    std::string value;
    if (is_secret_key(field.key)) {
      value = "***";
    } else {
      const bool path_like =
          key.find("name") != std::string::npos ||
          key.find("path") != std::string::npos ||
          key.find("file") != std::string::npos ||
          key.find("dest") != std::string::npos;
      value = truncate_utf8(field.value, path_like ? kPathLimit : kFieldLimit);
    }
    line.push_back(' ');
    line += key;
    line.push_back('=');
    line += quote_value(value);
  }
  if (!message.empty()) {
    line += " :: ";
    line += truncate_utf8(sanitize_message(message), kMessageLimit);
  }
  return truncate_utf8(line, kLineLimit);
}

void diag_log::push_ring_locked(const std::string& line) {
  ring_.push_back(line);
  while (ring_.size() > ring_capacity_) ring_.pop_front();
}

void diag_log::open_for_day_locked(int64_t now) {
  if (!enabled_ || file_broken_ || directory_.empty()) return;
  const int day = day_number(now);
  if (open_day_ == day && !current_path_.empty()) return;

  current_path_.clear();
  current_bytes_ = 0;
  open_day_ = day;

  std::error_code ec;
  fs::create_directories(directory_, ec);
  if (ec) {
    file_broken_ = true;
    last_error_ = ec.message();
    return;
  }
  const fs::path path = fs::path(directory_) / file_name_for_day(now);

  // Cleanup is cheap and happens at most once per day.
  if (last_cleanup_day_ != day) {
    last_cleanup_day_ = day;
    cleanup_old_files_locked(day);
  }

  current_path_ = path.string();
  std::error_code size_ec;
  const auto size = fs::file_size(path, size_ec);
  current_bytes_ = size_ec ? 0 : static_cast<uint64_t>(size);
  // Opening with append happens on the first write; no truncation here, so a
  // crash mid-line only costs that line.
}

void diag_log::rotate_locked() {
  if (current_path_.empty()) return;
  const fs::path path(current_path_);
  fs::path rotated = path;
  rotated += ".1";
  std::error_code ec;
  fs::remove(rotated, ec);
  ec.clear();
  fs::rename(path, rotated, ec);
  if (ec) {
    // Rename failed (locked file, permissions): keep appending instead of
    // losing lines. The file then exceeds the soft cap.
    return;
  }
  current_bytes_ = 0;
}

void diag_log::write_locked(const std::string& line, int64_t now) {
  if (!enabled_ || file_broken_) return;
  open_for_day_locked(now);
  if (current_path_.empty() || file_broken_) return;

  const uint64_t needed = static_cast<uint64_t>(line.size() + 1);
  if (current_bytes_ > 0 && current_bytes_ + needed > max_file_bytes_) {
    rotate_locked();
  }
  std::ofstream out(current_path_,
                    std::ios::binary | std::ios::app | std::ios::out);
  if (!out) {
    file_broken_ = true;
    return;
  }
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
  out.put('\n');
  out.flush();
  if (!out) {
    file_broken_ = true;
    return;
  }
  current_bytes_ += needed;
}

void diag_log::log(diag_level level, const std::string& event,
                   const std::string& message,
                   const std::vector<diag_field>& fields) {
  const int64_t now = now_ms();
  const std::string line = format_line(now, level, event, message, fields);
  std::lock_guard<std::mutex> lock(mutex_);
  push_ring_locked(line);
  write_locked(line, now);
}

std::string diag_log::tail(int lines) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t want = static_cast<std::size_t>(std::max(0, lines));
  if (want == 0) want = 500;
  want = std::min<std::size_t>(want, 5000);

  std::vector<std::string> collected;
  bool truncated = false;

  std::vector<log_file_entry> files = list_log_files(directory_);
  // Newest day first; within one day the plain file is newer than `.1`.
  std::sort(files.begin(), files.end(),
            [](const log_file_entry& a, const log_file_entry& b) {
              if (a.day != b.day) return a.day > b.day;
              return a.part < b.part;
            });
  if (!files.empty()) {
    std::size_t need = want;
    for (const log_file_entry& entry : files) {
      if (need == 0) break;
      std::vector<std::string> part = read_last_lines(entry.path, need);
      if (part.size() > need) part.resize(need);
      collected.insert(collected.begin(), part.begin(), part.end());
      need -= part.size();
    }
  }

  // The ring holds every line this process logged, including lines written
  // while recording was off (or when a file write failed). It is therefore the
  // authoritative tail; the files only supply history from before the ring
  // started (an earlier process). Splice rather than concatenate: the last
  // `ring.size()` file lines are the same lines the ring has.
  const std::size_t ring_take = std::min(want, ring_.size());
  if (ring_take > 0) {
    std::vector<std::string> from_ring(
        ring_.end() - static_cast<std::ptrdiff_t>(ring_take), ring_.end());
    if (ring_take >= want) {
      collected = std::move(from_ring);
    } else {
      if (collected.size() > ring_take) {
        collected.resize(collected.size() - ring_take);
      } else {
        collected.clear();
      }
      collected.insert(collected.end(), from_ring.begin(), from_ring.end());
    }
  }

  // Cap by bytes from the newest end, so the newest lines always survive.
  std::size_t bytes = 0;
  std::size_t keep = 0;
  for (std::size_t i = collected.size(); i > 0; --i) {
    const std::size_t line_bytes = collected[i - 1].size() + 1;
    if (bytes + line_bytes > kTailByteLimit) {
      truncated = true;
      break;
    }
    bytes += line_bytes;
    ++keep;
  }
  if (keep < collected.size()) {
    collected.erase(collected.begin(),
                    collected.end() - static_cast<std::ptrdiff_t>(keep));
    truncated = true;
  }

  std::string out;
  if (truncated) {
    out += "[truncated to 256 KiB]\n";
  }
  for (std::size_t i = 0; i < collected.size(); ++i) {
    if (i != 0) out.push_back('\n');
    out += collected[i];
  }
  return out;
}

diag_log::info diag_log::describe() const {
  std::lock_guard<std::mutex> lock(mutex_);
  info result;
  result.directory = directory_;
  result.retention_days = retention_days_;
  result.enabled = enabled_;
  result.current_file = current_path_;
  if (!current_path_.empty()) {
    result.current_bytes = current_bytes_;
  } else {
    // Report the newest on-disk file even while disabled, so the settings
    // page can still show what is using space.
    std::vector<log_file_entry> files = list_log_files(directory_);
    std::sort(files.begin(), files.end(),
              [](const log_file_entry& a, const log_file_entry& b) {
                if (a.day != b.day) return a.day > b.day;
                return a.part < b.part;
              });
    if (!files.empty()) {
      std::error_code ec;
      const auto size = fs::file_size(files.front().path, ec);
      result.current_file = files.front().path.string();
      result.current_bytes = ec ? 0 : static_cast<uint64_t>(size);
    }
  }
  return result;
}

std::size_t diag_log::cleanup_old_files_locked(int today) {
  if (directory_.empty()) return 0;
  std::size_t removed = 0;
  const std::vector<log_file_entry> files = list_log_files(directory_);
  for (const log_file_entry& entry : files) {
    if (today - entry.day <= retention_days_) continue;
    std::error_code ec;
    if (fs::remove(entry.path, ec)) ++removed;
  }
  return removed;
}

std::size_t diag_log::cleanup_old_files() {
  std::lock_guard<std::mutex> lock(mutex_);
  // "Switched off" also means "do not delete what is already there": the
  // user keeps the old files until they clear them by hand.
  if (!enabled_) return 0;
  return cleanup_old_files_locked(day_number(now_ms()));
}

std::string default_diag_directory() {
  const std::string override_dir = env_value("MOLINK_DIAG_LOG_DIR");
  if (!override_dir.empty()) return override_dir;
#if defined(_WIN32)
  std::string base = env_value("PROGRAMDATA");
  if (base.empty()) base = "C:\\ProgramData";
  const std::string product = base + "\\moLink";
  std::error_code ec;
  // The installer creates %ProgramData%\moLink with permissions for the
  // service and the desktop user. A portable run by a normal user cannot
  // create it, so use the per-user data directory instead of failing every
  // write. (No probe file is written here: logging may be switched off.)
  if (fs::is_directory(product, ec)) return product + "\\logs";
  const std::string local = env_value("LOCALAPPDATA");
  if (!local.empty()) return local + "\\moLink\\logs";
  const std::string appdata = env_value("APPDATA");
  if (!appdata.empty()) return appdata + "\\moLink\\logs";
  return product + "\\logs";
#elif defined(__APPLE__)
  const std::string home = env_value("HOME");
  return home.empty() ? std::string()
                      : home + "/Library/Logs/moLink";
#else
  const std::string xdg = env_value("XDG_DATA_HOME");
  if (!xdg.empty()) return xdg + "/molink/logs";
  const std::string home = env_value("HOME");
  return home.empty() ? std::string()
                      : home + "/.local/share/molink/logs";
#endif
}

diag_log& diag() {
  static diag_log instance(default_diag_directory(), true);
  return instance;
}

}  // namespace molink
