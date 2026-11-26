#include "download_root.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "util.h"

namespace molink {
namespace {

namespace fs = std::filesystem;

/// Device ids arrive as 64-character client identities; longer names are
/// truncated on a UTF-8 boundary so the directory stays portable.
constexpr std::size_t kMaxComponentBytes = 64;

std::string env_value(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string();
}

std::string trim_copy(const std::string& value) {
  std::size_t begin = 0;
  std::size_t end = value.size();
  while (begin < end && (value[begin] == ' ' || value[begin] == '\t' ||
                         value[begin] == '\r' || value[begin] == '\n')) {
    ++begin;
  }
  while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                         value[end - 1] == '\r' || value[end - 1] == '\n')) {
    --end;
  }
  return value.substr(begin, end - begin);
}

bool is_illegal_component_byte(unsigned char byte) {
  if (byte < 0x20 || byte == 0x7f) return true;
  switch (byte) {
    case '\\':
    case '/':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
      return true;
    default:
      return false;
  }
}

/// Absolute, symlink-resolved when possible, with `\` normalised to `/` so the
/// Windows and POSIX comparisons share one implementation.
std::string comparable_path(const fs::path& path) {
  std::error_code ec;
  fs::path resolved = fs::weakly_canonical(path, ec);
  if (ec) {
    resolved = path.lexically_normal();
  }
  std::string text = path_to_utf8(resolved);
  for (char& ch : text) {
    if (ch == '\\') ch = '/';
  }
  while (text.size() > 1 && text.back() == '/') text.pop_back();
#if defined(_WIN32)
  for (char& ch : text) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
#endif
  return text;
}

/// True when @p candidate is @p parent or lives underneath it. Both paths must
/// already be absolute.
bool is_within(const fs::path& candidate, const fs::path& parent) {
  const std::string child = comparable_path(candidate);
  std::string root = comparable_path(parent);
  if (child.empty() || root.empty()) return false;
  if (child == root) return true;
  if (root == "/") return true;
  if (root.back() != '/') root.push_back('/');
  return child.compare(0, root.size(), root) == 0;
}

bool is_directory_writable(const fs::path& path) {
#if defined(_WIN32)
  return _waccess(path.c_str(), 2) == 0;
#else
  return ::access(path.c_str(), W_OK) == 0;
#endif
}

std::vector<fs::path> system_directories() {
  std::vector<fs::path> roots;
#if defined(_WIN32)
  const std::string windows = env_value("SystemRoot");
  if (!windows.empty()) roots.push_back(path_from_utf8(windows));
  const std::string program_files = env_value("ProgramFiles");
  if (!program_files.empty()) roots.push_back(path_from_utf8(program_files));
  const std::string program_files_x86 = env_value("ProgramFiles(x86)");
  if (!program_files_x86.empty()) {
    roots.push_back(path_from_utf8(program_files_x86));
  }
  const std::string program_data = env_value("ProgramData");
  if (!program_data.empty()) roots.push_back(path_from_utf8(program_data));
  // The environment can be stripped (a service account); keep the usual
  // locations as literals as well.
  roots.push_back(path_from_utf8("C:\\Windows"));
  roots.push_back(path_from_utf8("C:\\Program Files"));
  roots.push_back(path_from_utf8("C:\\Program Files (x86)"));
  roots.push_back(path_from_utf8("C:\\ProgramData"));
#else
  for (const char* path : {"/usr", "/etc", "/bin", "/sbin", "/lib", "/lib32",
                           "/lib64", "/libx32", "/boot", "/sys", "/proc",
                           "/dev", "/var", "/System", "/Library",
                           "/Applications", "/private", "/cores"}) {
    roots.push_back(fs::path(path));
  }
#endif
  return roots;
}

bool is_filesystem_root(const fs::path& path) {
  const fs::path normal = path.lexically_normal();
  return !normal.root_path().empty() && normal == normal.root_path();
}

download_root_check failure(download_root_error error, std::string message) {
  download_root_check check;
  check.ok = false;
  check.error = error;
  check.message = std::move(message);
  return check;
}

}  // namespace

std::string default_download_root() {
  std::string home;
#if defined(_WIN32)
  home = env_value("USERPROFILE");
  if (home.empty()) {
    const std::string drive = env_value("HOMEDRIVE");
    const std::string path = env_value("HOMEPATH");
    if (!drive.empty() && !path.empty()) home = drive + path;
  }
#else
  home = env_value("HOME");
#endif
  if (home.empty()) return std::string();
  return path_to_utf8(path_from_utf8(home) / "moLink");
}

std::string resolve_download_root(const std::string& configured) {
  const std::string trimmed = trim_copy(configured);
  if (!trimmed.empty()) return trimmed;
  return default_download_root();
}

const char* download_root_error_name(download_root_error error) {
  switch (error) {
    case download_root_error::none:
      return "ok";
    case download_root_error::empty:
      return "empty";
    case download_root_error::relative:
      return "relative_path";
    case download_root_error::system_directory:
      return "system_directory";
    case download_root_error::install_directory:
      return "install_directory";
    case download_root_error::not_directory:
      return "not_a_directory";
    case download_root_error::not_writable:
      return "not_writable";
  }
  return "unknown";
}

download_root_check validate_download_root(const std::string& root) {
  const std::string trimmed = trim_copy(root);
  if (trimmed.empty()) {
    return failure(download_root_error::empty,
                   "the storage location is empty");
  }
  const fs::path path = path_from_utf8(trimmed);
  if (!path.is_absolute()) {
    return failure(download_root_error::relative,
                   "the storage location must be an absolute path");
  }
  if (is_filesystem_root(path)) {
    return failure(download_root_error::system_directory,
                   "a filesystem root cannot be the storage location");
  }
  for (const fs::path& system : system_directories()) {
    if (is_within(path, system)) {
      return failure(download_root_error::system_directory,
                     "the storage location is a system directory");
    }
  }
  const std::string install = program_directory();
  if (!install.empty() &&
      is_within(path, path_from_utf8(install))) {
    return failure(download_root_error::install_directory,
                   "the storage location is the program's install directory");
  }

  std::error_code ec;
  if (fs::exists(path, ec)) {
    if (!fs::is_directory(path, ec)) {
      return failure(download_root_error::not_directory,
                     "the storage location is not a directory");
    }
    if (!is_directory_writable(path)) {
      return failure(download_root_error::not_writable,
                     "the storage location is not writable");
    }
  } else {
    // The folder is created on the first received file, so a missing leaf is
    // fine as long as its nearest existing parent can be written to. Nothing
    // is created here.
    fs::path probe = path;
    while (!probe.empty() && !fs::exists(probe, ec)) {
      const fs::path parent = probe.parent_path();
      if (parent == probe) break;
      probe = parent;
    }
    if (probe.empty() || !fs::is_directory(probe, ec) ||
        !is_directory_writable(probe)) {
      return failure(download_root_error::not_writable,
                     "the storage location cannot be created; its parent is "
                     "not writable");
    }
  }

  download_root_check check;
  check.ok = true;
  check.error = download_root_error::none;
  return check;
}

std::string sanitize_device_component(const std::string& component) {
  std::string out;
  out.reserve(component.size());
  for (unsigned char byte : component) {
    if (is_illegal_component_byte(byte)) {
      out.push_back('_');
    } else {
      out.push_back(static_cast<char>(byte));
    }
  }
  if (out.size() > kMaxComponentBytes) {
    std::size_t cut = kMaxComponentBytes;
    // Never split a multi-byte UTF-8 sequence.
    while (cut > 0 &&
           (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) {
      --cut;
    }
    out.resize(cut);
  }
  // Windows cannot use a directory name with a trailing space or dot; trimming
  // also keeps `.`/`..` from naming the parent.
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
    out.pop_back();
  }
  while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
    out.erase(out.begin());
  }
  if (out.empty() || out == "." || out == "..") return "unknown";
  return out;
}

std::string local_date_string(std::time_t when) {
  // `std::localtime` reuses one static buffer and MSVC/MinGW disagree on the
  // argument order of their `_s` flavours, so take the portable call under a
  // lock (the same rule diag_log.cpp follows).
  static std::mutex time_mutex;
  std::tm local{};
  {
    std::lock_guard<std::mutex> lock(time_mutex);
    const std::tm* value = std::localtime(&when);
    if (value == nullptr) return std::string();
    local = *value;
  }
  char buffer[16] = {};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local) == 0) {
    return std::string();
  }
  return buffer;
}

std::string download_directory(const std::string& root,
                               const std::string& device_id,
                               const std::string& date) {
  if (root.empty()) return std::string();
  const std::string day =
      date.empty() ? local_date_string()
                   : sanitize_device_component(date);
  const fs::path directory =
      path_from_utf8(root) /
      path_from_utf8(sanitize_device_component(device_id)) /
      path_from_utf8(day);
  return path_to_utf8(directory.lexically_normal());
}

std::string chat_attachment_dir(const std::string& root,
                                const std::string& device_id,
                                const std::string& date) {
  const std::string base = download_directory(root, device_id, date);
  if (base.empty()) return std::string();
  return path_to_utf8(path_from_utf8(base) / "chat");
}

std::string unique_path(const std::string& directory,
                        const std::string& name) {
  if (directory.empty() || name.empty()) return std::string();
  const fs::path dir = path_from_utf8(directory);
  const fs::path name_path = path_from_utf8(name);
  fs::path candidate = dir / name_path;
  std::error_code ec;
  if (!fs::exists(candidate, ec)) return path_to_utf8(candidate);

  const std::string stem = path_to_utf8(name_path.stem());
  const std::string extension = path_to_utf8(name_path.extension());
  for (int index = 1; index < 10000; ++index) {
    candidate = dir / path_from_utf8(stem + " (" + std::to_string(index) +
                                     ")" + extension);
    if (!fs::exists(candidate, ec)) return path_to_utf8(candidate);
  }
  const std::string stamp = std::to_string(
      static_cast<long long>(std::time(nullptr)));
  return path_to_utf8(dir / path_from_utf8(stem + "-" + stamp + extension));
}

std::string program_directory() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return std::string();
  const fs::path exe(std::wstring(buffer.data(), length));
  return path_to_utf8(exe.parent_path());
#elif defined(__APPLE__)
  char buffer[4096] = {};
  uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) != 0) return std::string();
  std::error_code ec;
  const fs::path exe = fs::weakly_canonical(fs::path(buffer), ec);
  if (ec) return std::string();
  return path_to_utf8(exe.parent_path());
#elif defined(__linux__)
  char buffer[4096] = {};
  const ssize_t length = ::readlink("/proc/self/exe", buffer,
                                    sizeof(buffer) - 1);
  if (length <= 0) return std::string();
  const fs::path exe(std::string(buffer, static_cast<std::size_t>(length)));
  return path_to_utf8(exe.parent_path());
#else
  return std::string();
#endif
}

}  // namespace molink
