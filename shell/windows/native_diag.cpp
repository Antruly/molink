#include "native_diag.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace molink::native_diag {
namespace {

constexpr std::size_t kFieldLimit = 300;
constexpr std::size_t kPathLimit = 120;
constexpr std::size_t kMessageLimit = 1000;
constexpr std::size_t kLineLimit = 4096;
constexpr std::size_t kMaxGatherLines = 5000;

const char* kFilePrefix = "molink-";
const char* kFileSuffix = ".log";

#if defined(_WIN32)
constexpr char kSeparator = '\\';

std::wstring widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0);
  if (size <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), size);
  return out;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) return std::string();
  std::string out(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}
#else
constexpr char kSeparator = '/';
#endif

std::string env_value(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

std::string join_path(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  if (directory.back() == '/' || directory.back() == '\\') {
    return directory + name;
  }
  return directory + kSeparator + name;
}

#if defined(_WIN32)
bool make_dirs(const std::string& directory) {
  if (directory.empty()) return false;
  const std::wstring wide = widen(directory);
  if (wide.empty()) return false;
  for (std::size_t i = 1; i <= wide.size(); ++i) {
    if (i != wide.size() && wide[i] != L'\\' && wide[i] != L'/') continue;
    const std::wstring part = wide.substr(0, i);
    if (part.size() == 2 && part[1] == L':') continue;
    const DWORD attributes = GetFileAttributesW(part.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      CreateDirectoryW(part.c_str(), nullptr);
    } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return false;
    }
  }
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool is_directory(const std::string& directory) {
  const DWORD attributes = GetFileAttributesW(widen(directory).c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// File names in one directory (directories skipped).
std::vector<std::string> list_directory(const std::string& directory) {
  std::vector<std::string> names;
  if (directory.empty()) return names;
  const std::wstring pattern = widen(join_path(directory, "*"));
  WIN32_FIND_DATAW data{};
  HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) return names;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    names.push_back(narrow(data.cFileName));
  } while (FindNextFileW(handle, &data) != 0);
  FindClose(handle);
  return names;
}

bool file_size(const std::string& path, uint64_t* out) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard,
                           &data) == 0) {
    return false;
  }
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
  *out = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
         data.nFileSizeLow;
  return true;
}

bool remove_file(const std::string& path) {
  return DeleteFileW(widen(path).c_str()) != 0;
}

bool rename_replace(const std::string& from, const std::string& to) {
  return MoveFileExW(widen(from).c_str(), widen(to).c_str(),
                     MOVEFILE_REPLACE_EXISTING) != 0;
}

class file_reader {
 public:
  explicit file_reader(const std::string& path) {
    handle_ = CreateFileW(widen(path).c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE |
                              FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      handle_ = nullptr;
      return;
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle_, &size) == 0) {
      CloseHandle(handle_);
      handle_ = nullptr;
      return;
    }
    size_ = static_cast<uint64_t>(size.QuadPart);
  }

  ~file_reader() {
    if (handle_ != nullptr) CloseHandle(handle_);
  }

  file_reader(const file_reader&) = delete;
  file_reader& operator=(const file_reader&) = delete;

  bool ok() const { return handle_ != nullptr; }
  uint64_t size() const { return size_; }

  bool read_at(uint64_t offset, char* data, std::size_t length) {
    if (handle_ == nullptr) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) == 0) {
      return false;
    }
    std::size_t total = 0;
    while (total < length) {
      DWORD got = 0;
      const DWORD want =
          static_cast<DWORD>((std::min)(length - total,
                                        static_cast<std::size_t>(1u << 30)));
      if (ReadFile(handle_, data + total, want, &got, nullptr) == 0 ||
          got == 0) {
        return false;
      }
      total += got;
    }
    return true;
  }

 private:
  HANDLE handle_ = nullptr;
  uint64_t size_ = 0;
};

bool append_line(const std::string& path, const std::string& line) {
  HANDLE handle = CreateFileW(
      widen(path).c_str(), FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  std::string bytes = line;
  bytes.push_back('\n');
  DWORD written = 0;
  const BOOL ok = WriteFile(handle, bytes.data(),
                            static_cast<DWORD>(bytes.size()), &written,
                            nullptr);
  CloseHandle(handle);
  return ok != 0 && written == bytes.size();
}
#else
bool make_dirs(const std::string& directory) {
  if (directory.empty()) return false;
  std::string current;
  std::size_t index = 0;
  if (directory[0] == '/') {
    current = "/";
    index = 1;
  }
  while (index <= directory.size()) {
    std::size_t next = directory.find('/', index);
    if (next == std::string::npos) next = directory.size();
    if (next > index) {
      if (!current.empty() && current.back() != '/') current.push_back('/');
      current.append(directory, index, next - index);
      if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    if (next == directory.size()) break;
    index = next + 1;
  }
  struct stat info {};
  return stat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool is_directory(const std::string& directory) {
  struct stat info {};
  return stat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::vector<std::string> list_directory(const std::string& directory) {
  std::vector<std::string> names;
  DIR* dir = opendir(directory.c_str());
  if (dir == nullptr) return names;
  while (dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    struct stat info {};
    if (stat(join_path(directory, name).c_str(), &info) != 0) continue;
    if (!S_ISREG(info.st_mode)) continue;
    names.push_back(name);
  }
  closedir(dir);
  return names;
}

bool file_size(const std::string& path, uint64_t* out) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
  *out = static_cast<uint64_t>(info.st_size);
  return true;
}

bool remove_file(const std::string& path) {
  return std::remove(path.c_str()) == 0;
}

bool rename_replace(const std::string& from, const std::string& to) {
  return std::rename(from.c_str(), to.c_str()) == 0;
}

class file_reader {
 public:
  explicit file_reader(const std::string& path)
      : in_(path, std::ios::binary) {
    if (!in_) return;
    in_.seekg(0, std::ios::end);
    const std::streamoff size = in_.tellg();
    if (size < 0) {
      in_.clear();
      in_.setstate(std::ios::failbit);
      return;
    }
    size_ = static_cast<uint64_t>(size);
    ok_ = true;
  }

  bool ok() const { return ok_; }
  uint64_t size() const { return size_; }

  bool read_at(uint64_t offset, char* data, std::size_t length) {
    if (!ok_) return false;
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    in_.read(data, static_cast<std::streamsize>(length));
    return in_.gcount() == static_cast<std::streamsize>(length);
  }

 private:
  std::ifstream in_;
  uint64_t size_ = 0;
  bool ok_ = false;
};

bool append_line(const std::string& path, const std::string& line) {
  std::ofstream out(path, std::ios::binary | std::ios::app | std::ios::out);
  if (!out) return false;
  out.write(line.data(), static_cast<std::streamsize>(line.size()));
  out.put('\n');
  out.flush();
  return static_cast<bool>(out);
}
#endif

int64_t system_now_ms() {
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

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

std::time_t utc_from_tm(std::tm* tm_value) {
#if defined(_WIN32)
  return _mkgmtime(tm_value);
#else
  return timegm(tm_value);
#endif
}

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
  std::tm copy = local_tm;
  const int64_t local_as_utc = static_cast<int64_t>(utc_from_tm(&copy));
  out.offset_minutes = static_cast<int>((local_as_utc - seconds) / 60);
  return out;
}

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
  while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xc0) == 0x80) {
    --cut;
  }
  return value.substr(0, cut) + "...";
}

bool secret_key_name(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
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

/// The core names its events `component.event`; do the same here so both
/// sides produce identical lines, and never prefix twice.
std::string scoped_event(component part, const std::string& event) {
  if (event.find('.') != std::string::npos) return event;
  const std::string prefix = logger::component_name(part);
  if (prefix.empty()) return event;
  return prefix + "." + event;
}

struct log_file_entry {
  std::string path;
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
  if (directory.empty() || !is_directory(directory)) return files;
  for (const std::string& name : list_directory(directory)) {
    int day = 0;
    int part = 0;
    if (!parse_log_file_name(name, day, part)) continue;
    log_file_entry entry;
    entry.path = join_path(directory, name);
    entry.day = day;
    entry.part = part;
    files.push_back(std::move(entry));
  }
  return files;
}

/// Last `want` lines of one file, read backwards so a 32 MiB file is not
/// loaded whole for a twenty-line tail.
std::vector<std::string> read_last_lines(const std::string& path,
                                         std::size_t want) {
  std::vector<std::string> lines;
  if (want == 0) return lines;
  file_reader in(path);
  if (!in.ok() || in.size() == 0) return lines;
  constexpr std::size_t kChunk = 64 * 1024;
  std::string data;
  uint64_t pos = in.size();
  std::size_t newlines = 0;
  while (pos > 0 && newlines <= want) {
    const std::size_t take = static_cast<std::size_t>(
        (std::min)(static_cast<uint64_t>(kChunk), pos));
    pos -= take;
    std::string chunk(take, '\0');
    if (!in.read_at(pos, chunk.data(), take)) break;
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
  while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
  if (lines.size() > want) {
    lines.erase(lines.begin(),
                lines.end() - static_cast<std::ptrdiff_t>(want));
  }
  return lines;
}

}  // namespace

bool logger::level_at_least(level value, level minimum) {
  return static_cast<int>(value) >= static_cast<int>(minimum);
}

const char* logger::level_name(level value) {
  switch (value) {
    case level::trace:
      return "TRACE";
    case level::debug:
      return "DEBUG";
    case level::warn:
      return "WARN";
    case level::error:
      return "ERROR";
    case level::info:
    default:
      return "INFO";
  }
}

const char* logger::component_name(component value) {
  switch (value) {
    case component::transport:
      return "transport";
    case component::video:
      return "video";
    case component::clipboard:
      return "clipboard";
    case component::files:
      return "files";
    case component::input:
      return "input";
    case component::auth:
      return "auth";
    case component::ui:
      return "ui";
    case component::session:
    default:
      return "session";
  }
}

bool logger::keep_day(int file_day, int today_day, int retention_days) {
  if (retention_days < 1) retention_days = 1;
  return today_day - file_day <= retention_days;
}

logger::logger(std::string directory, bool enabled, int retention_days,
               uint64_t max_file_bytes, std::function<int64_t()> now_ms,
               std::size_t ring_lines)
    : directory_(std::move(directory)),
      enabled_(enabled),
      retention_days_((std::max)(1, retention_days)),
      max_file_bytes_(max_file_bytes > 0 ? max_file_bytes
                                         : kDefaultMaxFileBytes),
      now_ms_(std::move(now_ms)),
      ring_capacity_((std::max<std::size_t>)(1, ring_lines)) {}

logger::~logger() {
  std::lock_guard<std::mutex> lock(mutex_);
  current_path_.clear();
}

int64_t logger::now_ms() const {
  return now_ms_ ? now_ms_() : system_now_ms();
}

void logger::set_enabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  enabled_ = enabled;
  if (!enabled) {
    current_path_.clear();
    current_bytes_ = 0;
    open_day_ = -1;
    file_broken_ = false;
  }
}

bool logger::enabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return enabled_;
}

void logger::set_level(level minimum) {
  std::lock_guard<std::mutex> lock(mutex_);
  minimum_level_ = minimum;
}

level logger::minimum_level() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return minimum_level_;
}

bool logger::is_secret_key(const std::string& key) {
  return secret_key_name(key);
}

std::string logger::sanitize_message(const std::string& message) {
  std::string out = message;
  const std::string guard = "\x01KEYCODE\x01";
  static const std::regex key_phrase(R"(key\s+code)",
                                     std::regex_constants::icase);
  out = std::regex_replace(out, key_phrase, guard);
  static const std::regex labelled(
      R"((token|session|code|password|passwd|secret)\s*[=:]\s*[^\s,;]+)",
      std::regex_constants::icase);
  out = std::regex_replace(out, labelled, "$1=***");
  static const std::regex hex_run(R"(\b[0-9a-fA-F]{32,}\b)");
  out = std::regex_replace(out, hex_run, "***");
  static const std::regex code_shape(
      R"(\b[A-Z0-9]{3}-[A-Z0-9]{3}-[A-Z0-9]{3}\b)");
  out = std::regex_replace(out, code_shape, "***");
  std::size_t at = 0;
  while ((at = out.find(guard, at)) != std::string::npos) {
    out.replace(at, guard.size(), "key code");
    at += 8;
  }
  return out;
}

std::string logger::file_name_for_day(int64_t epoch_ms) {
  const local_time t = to_local(epoch_ms);
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "%s%04d%02d%02d%s", kFilePrefix,
                t.year, t.month, t.day, kFileSuffix);
  return std::string(buffer);
}

int logger::day_number(int64_t epoch_ms) {
  const local_time t = to_local(epoch_ms);
  return static_cast<int>(days_from_civil(t.year, t.month, t.day));
}

std::string logger::format_line(int64_t epoch_ms, level severity,
                                component part, const std::string& event,
                                const std::string& message,
                                const std::vector<field>& fields) {
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
  line += level_name(severity);
  line.push_back(' ');
  line += normalize_key(truncate_utf8(scoped_event(part, event), 60));
  const bool exit_event = event == "app.exit";
  for (const field& item : fields) {
    const std::string key = normalize_key(item.key);
    std::string value;
    if (is_secret_key(item.key) && !(exit_event && key == "code")) {
      value = "***";
    } else {
      const bool path_like = key.find("name") != std::string::npos ||
                             key.find("path") != std::string::npos ||
                             key.find("file") != std::string::npos ||
                             key.find("dest") != std::string::npos;
      value = truncate_utf8(item.value,
                            path_like ? kPathLimit : kFieldLimit);
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

level logger::line_level(const std::string& line) {
  const std::size_t first = line.find(' ');
  if (first == std::string::npos) return level::trace;
  const std::size_t second = line.find(' ', first + 1);
  const std::string token =
      line.substr(first + 1, second == std::string::npos
                                 ? std::string::npos
                                 : second - first - 1);
  if (token == "ERROR") return level::error;
  if (token == "WARN") return level::warn;
  if (token == "INFO") return level::info;
  if (token == "DEBUG") return level::debug;
  return level::trace;
}

void logger::push_ring_locked(const std::string& line) {
  ring_.push_back(line);
  while (ring_.size() > ring_capacity_) ring_.pop_front();
}

void logger::open_for_day_locked(int64_t now) {
  if (!enabled_ || file_broken_ || directory_.empty()) return;
  const int day = day_number(now);
  if (open_day_ == day && !current_path_.empty()) return;

  current_path_.clear();
  current_bytes_ = 0;
  open_day_ = day;

  if (!make_dirs(directory_)) {
    file_broken_ = true;
    return;
  }
  const std::string path = join_path(directory_, file_name_for_day(now));
  if (last_cleanup_day_ != day) {
    last_cleanup_day_ = day;
    cleanup_old_files_locked(day);
  }
  current_path_ = path;
  uint64_t size = 0;
  current_bytes_ = file_size(path, &size) ? size : 0;
}

void logger::rotate_locked() {
  if (current_path_.empty()) return;
  const std::string rotated = current_path_ + ".1";
  remove_file(rotated);
  if (!rename_replace(current_path_, rotated)) {
    // A locked file must not lose lines: keep appending and go over the soft
    // cap instead.
    return;
  }
  current_bytes_ = 0;
}

void logger::write_locked(const std::string& line, int64_t now) {
  if (!enabled_ || file_broken_) return;
  open_for_day_locked(now);
  if (current_path_.empty() || file_broken_) return;

  const uint64_t needed = static_cast<uint64_t>(line.size() + 1);
  if (current_bytes_ > 0 && current_bytes_ + needed > max_file_bytes_) {
    rotate_locked();
  }
  if (!append_line(current_path_, line)) {
    file_broken_ = true;
    return;
  }
  current_bytes_ += needed;
}

void logger::log(level severity, component part, const std::string& event,
                 const std::string& message,
                 const std::vector<field>& fields) {
  const int64_t now = now_ms();
  const std::string line =
      format_line(now, severity, part, event, message, fields);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!level_at_least(severity, minimum_level_)) return;
  push_ring_locked(line);
  write_locked(line, now);
}

std::vector<std::string> logger::gather_locked(std::size_t want) const {
  std::vector<std::string> collected;
  if (want == 0) return collected;

  std::vector<log_file_entry> files = list_log_files(directory_);
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

  const std::size_t ring_take = (std::min)(want, ring_.size());
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
  return collected;
}

std::string logger::render_locked(std::vector<std::string> lines) const {
  std::size_t bytes = 0;
  std::size_t keep = 0;
  bool truncated = false;
  for (std::size_t i = lines.size(); i > 0; --i) {
    const std::size_t line_bytes = lines[i - 1].size() + 1;
    if (bytes + line_bytes > kTailByteLimit) {
      truncated = true;
      break;
    }
    bytes += line_bytes;
    ++keep;
  }
  if (keep < lines.size()) {
    lines.erase(lines.begin(),
                lines.end() - static_cast<std::ptrdiff_t>(keep));
    truncated = true;
  }
  std::string out;
  if (truncated) out += "[truncated to 256 KiB]\n";
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) out.push_back('\n');
    out += lines[i];
  }
  return out;
}

std::string logger::tail(int lines) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t want = static_cast<std::size_t>((std::max)(0, lines));
  if (want == 0) want = 500;
  want = (std::min)(want, kMaxGatherLines);
  return render_locked(gather_locked(want));
}

std::string logger::tail(int lines, level minimum) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t want = static_cast<std::size_t>((std::max)(0, lines));
  if (want == 0) want = 500;
  want = (std::min)(want, kMaxGatherLines);
  std::size_t gather = want * 4;
  if (gather < want) gather = want;
  gather = (std::min)(gather, kMaxGatherLines);
  std::vector<std::string> collected = gather_locked(gather);
  std::vector<std::string> filtered;
  filtered.reserve(collected.size());
  for (const std::string& line : collected) {
    if (level_at_least(line_level(line), minimum)) {
      filtered.push_back(line);
    }
  }
  if (filtered.size() > want) {
    filtered.erase(filtered.begin(),
                   filtered.end() - static_cast<std::ptrdiff_t>(want));
  }
  return render_locked(std::move(filtered));
}

logger::state_info logger::describe() const {
  std::lock_guard<std::mutex> lock(mutex_);
  state_info result;
  result.directory = directory_;
  result.retention_days = retention_days_;
  result.enabled = enabled_;
  result.minimum = minimum_level_;
  result.current_file = current_path_;
  if (!current_path_.empty()) {
    result.current_bytes = current_bytes_;
  } else {
    std::vector<log_file_entry> files = list_log_files(directory_);
    std::sort(files.begin(), files.end(),
              [](const log_file_entry& a, const log_file_entry& b) {
                if (a.day != b.day) return a.day > b.day;
                return a.part < b.part;
              });
    if (!files.empty()) {
      result.current_file = files.front().path;
      uint64_t size = 0;
      result.current_bytes = file_size(files.front().path, &size) ? size : 0;
    }
  }
  return result;
}

bool logger::ensure_directory() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (directory_.empty()) return false;
  return make_dirs(directory_);
}

std::size_t logger::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t removed = 0;
  for (const log_file_entry& entry : list_log_files(directory_)) {
    if (remove_file(entry.path)) ++removed;
  }
  ring_.clear();
  current_path_.clear();
  current_bytes_ = 0;
  open_day_ = -1;
  file_broken_ = false;
  return removed;
}

std::size_t logger::cleanup_old_files_locked(int today) {
  if (directory_.empty()) return 0;
  std::size_t removed = 0;
  for (const log_file_entry& entry : list_log_files(directory_)) {
    if (keep_day(entry.day, today, retention_days_)) continue;
    if (remove_file(entry.path)) ++removed;
  }
  return removed;
}

std::size_t logger::cleanup_old_files() {
  std::lock_guard<std::mutex> lock(mutex_);
  // "Switched off" also means "do not delete what is already there": the user
  // keeps old files until they clear them by hand.
  if (!enabled_) return 0;
  return cleanup_old_files_locked(day_number(now_ms()));
}

std::string default_directory() {
  const std::string override_dir = env_value("MOLINK_DIAG_LOG_DIR");
  if (!override_dir.empty()) return override_dir;
#if defined(_WIN32)
  const std::string local = env_value("LOCALAPPDATA");
  if (!local.empty()) return local + "\\moLink\\logs";
  const std::string roaming = env_value("APPDATA");
  if (!roaming.empty()) return roaming + "\\moLink\\logs";
  return std::string();
#elif defined(__APPLE__)
  const std::string home = env_value("HOME");
  return home.empty() ? std::string() : home + "/Library/Logs/moLink";
#else
  const std::string xdg = env_value("XDG_DATA_HOME");
  if (!xdg.empty()) return xdg + "/molink/logs";
  const std::string home = env_value("HOME");
  return home.empty() ? std::string()
                      : home + "/.local/share/molink/logs";
#endif
}

logger& diag() {
  static logger instance(default_directory(), true);
  return instance;
}

}  // namespace molink::native_diag
